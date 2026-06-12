// malibu-view/src/view.cpp
// MalibuView: end-to-end embedding engine (HTML -> DOM -> CSS -> JS -> layout
// -> raster) with eval, messaging, request interception, and sandboxing.

#include "malibu/view/view.h"
#include "malibu/html/html_parser.h"
#include "malibu/css/parser/css_parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <tuple>

namespace malibu::view {
namespace {
// UTF-16 -> UTF-8 (proper encoding, incl. surrogate pairs).
std::string narrow(const std::u16string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) r.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { r.push_back(static_cast<char>(0xC0 | (cp >> 6))); r.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { r.push_back(static_cast<char>(0xE0 | (cp >> 12))); r.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); r.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { r.push_back(static_cast<char>(0xF0 | (cp >> 18))); r.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); r.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); r.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    return r;
}
// UTF-8 -> UTF-16 (proper decoding; invalid sequences become U+FFFD). Web bytes
// are UTF-8 by default, so this is what makes accents/CJK/emoji render at all.
std::u16string widen(const std::string& s) {
    std::u16string out; out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + static_cast<size_t>(extra) >= n) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        i += static_cast<size_t>(extra) + 1;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out.push_back(0xFFFD); continue; }
        if (cp <= 0xFFFF) out.push_back(static_cast<char16_t>(cp));
        else { cp -= 0x10000; out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10))); out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
    }
    return out;
}

void set_import_meta(js::Engine& engine, const std::string& url) {
    auto& interpreter = engine.interpreter();
    js::runtime::JSObject* meta = interpreter.new_object();
    meta->set(u"url", interpreter.str(url));
    interpreter.global()->define(
        u"__importMeta",
        js::runtime::Value::make_heap_ptr(meta));
}

std::string form_urlencode(std::u16string_view value) {
    const std::string bytes = narrow(std::u16string(value));
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(bytes.size());
    for (unsigned char byte : bytes) {
        if ((byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '*' ||
            byte == '-' || byte == '.' || byte == '_') {
            encoded.push_back(static_cast<char>(byte));
        } else if (byte == ' ') {
            encoded.push_back('+');
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4]);
            encoded.push_back(hex[byte & 0x0F]);
        }
    }
    return encoded;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decode_data_url(const std::string& url, std::string& body) {
    if (url.rfind("data:", 0) != 0) return false;
    const size_t comma = url.find(',');
    if (comma == std::string::npos) return false;
    const std::string metadata = url.substr(5, comma - 5);
    const std::string payload = url.substr(comma + 1);
    const bool base64 =
        metadata.size() >= 7 &&
        metadata.rfind(";base64") == metadata.size() - 7;

    std::string decoded;
    decoded.reserve(payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] == '%' && i + 2 < payload.size()) {
            const int high = hex_digit(payload[i + 1]);
            const int low = hex_digit(payload[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(
                    static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(payload[i]);
    }
    if (!base64) {
        body = std::move(decoded);
        return true;
    }

    signed char table[256];
    std::fill(std::begin(table), std::end(table),
              static_cast<signed char>(-1));
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < alphabet.size(); ++i)
        table[static_cast<unsigned char>(alphabet[i])] =
            static_cast<signed char>(i);
    body.clear();
    unsigned int bits = 0;
    int bit_count = 0;
    for (unsigned char c : decoded) {
        if (c == '=') break;
        const int value = table[c];
        if (value < 0) {
            if (std::isspace(c)) continue;
            return false;
        }
        bits = (bits << 6) | static_cast<unsigned int>(value);
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            body.push_back(static_cast<char>(
                (bits >> bit_count) & 0xFF));
        }
    }
    return true;
}

std::string trim_ascii_lower(std::u16string_view value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(
               static_cast<unsigned char>(value[first] & 0xFF))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::isspace(
               static_cast<unsigned char>(value[last - 1] & 0xFF))) {
        --last;
    }
    std::string out;
    out.reserve(last - first);
    for (size_t i = first; i < last; ++i) {
        char c = static_cast<char>(value[i] & 0xFF);
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    }
    return out;
}

std::u16string escape_css_identifier(std::u16string_view value) {
    std::u16string escaped;
    auto append_hex_escape = [&](char16_t c) {
        static constexpr char16_t digits[] = u"0123456789abcdef";
        escaped.push_back(u'\\');
        unsigned int code = c;
        char16_t buffer[4];
        size_t count = 0;
        do {
            buffer[count++] = digits[code & 0xF];
            code >>= 4;
        } while (code != 0);
        while (count > 0) escaped.push_back(buffer[--count]);
        escaped.push_back(u' ');
    };

    for (size_t i = 0; i < value.size(); ++i) {
        const char16_t c = value[i];
        if (c == 0) {
            escaped.push_back(0xFFFD);
            continue;
        }
        const bool control = (c >= 1 && c <= 0x1F) || c == 0x7F;
        const bool leading_digit = i == 0 && c >= u'0' && c <= u'9';
        const bool second_digit =
            i == 1 && value[0] == u'-' && c >= u'0' && c <= u'9';
        if (control || leading_digit || second_digit) {
            append_hex_escape(c);
            continue;
        }
        if (i == 0 && c == u'-' && value.size() == 1) {
            escaped += u"\\-";
            continue;
        }
        const bool safe = c >= 0x80 || c == u'-' || c == u'_' ||
                          (c >= u'0' && c <= u'9') ||
                          (c >= u'A' && c <= u'Z') ||
                          (c >= u'a' && c <= u'z');
        if (!safe) escaped.push_back(u'\\');
        escaped.push_back(c);
    }
    return escaped;
}

bool is_classic_script_type(const std::string& type) {
    if (type.empty()) return true;
    static constexpr const char* kJavaScriptTypes[] = {
        "application/ecmascript",
        "application/javascript",
        "application/x-ecmascript",
        "application/x-javascript",
        "text/ecmascript",
        "text/javascript",
        "text/javascript1.0",
        "text/javascript1.1",
        "text/javascript1.2",
        "text/javascript1.3",
        "text/javascript1.4",
        "text/javascript1.5",
        "text/jscript",
        "text/livescript",
        "text/x-ecmascript",
        "text/x-javascript",
    };
    return std::any_of(std::begin(kJavaScriptTypes),
                       std::end(kJavaScriptTypes),
                       [&](const char* candidate) { return type == candidate; });
}

bool is_http_error(const network::FetchResponse& response) {
    return response.status >= 400;
}

std::string resolve_against(const std::string& base, std::string ref) {
    const size_t first = ref.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return base;
    const size_t last = ref.find_last_not_of(" \t\r\n");
    ref = ref.substr(first, last - first + 1);

    const size_t colon = ref.find(':');
    const size_t slash = ref.find('/');
    if (colon != std::string::npos &&
        (slash == std::string::npos || colon < slash))
        return ref;
    const size_t scheme = base.find("://");
    if (ref.rfind("//", 0) == 0)
        return (scheme == std::string::npos ? "https:" :
                base.substr(0, scheme) + ":") + ref;
    if (!ref.empty() && ref[0] == '/' && scheme != std::string::npos) {
        const size_t path = base.find('/', scheme + 3);
        return (path == std::string::npos ? base : base.substr(0, path)) + ref;
    }
    if (!ref.empty() && (ref[0] == '?' || ref[0] == '#')) {
        const size_t marker = base.find_first_of("?#");
        return (marker == std::string::npos ? base : base.substr(0, marker)) + ref;
    }
    const size_t marker = base.find_first_of("?#");
    const std::string clean =
        marker == std::string::npos ? base : base.substr(0, marker);
    const size_t directory = clean.rfind('/');
    return directory == std::string::npos
        ? ref
        : clean.substr(0, directory + 1) + ref;
}

using js::runtime::Interpreter;
using js::runtime::JSArray;
using js::runtime::JSArrayBuffer;
using js::runtime::JSBigInt;
using js::runtime::JSDataView;
using js::runtime::JSFunction;
using js::runtime::JSMap;
using js::runtime::JSObject;
using js::runtime::JSSet;
using js::runtime::JSString;
using js::runtime::JSTypedArray;
using js::runtime::TAKind;
using js::runtime::ThrowSignal;
using js::runtime::Value;
using js::vm::HeapObject;

Value clone_between(
    Interpreter& source,
    Interpreter& target,
    Value value,
    std::vector<std::pair<HeapObject*, Value>>& seen) {
    if (!value.is_heap_ptr()) return value;
    HeapObject* object = value.as_heap_ptr();
    if (object->kind == HeapObject::kJSString)
        return target.str(static_cast<JSString*>(object)->data);
    if (object->kind == HeapObject::kJSBigInt)
        return Value::make_heap_ptr(
            target.new_bigint(static_cast<JSBigInt*>(object)->value));
    for (const auto& [original, copy] : seen)
        if (original == object) return copy;

    auto clone_properties = [&](JSObject* from, JSObject* to,
                                Value from_value) {
        for (const auto& property : from->props) {
            if (!property.enumerable) continue;
            Value property_value = property.is_accessor
                ? source.get_prop_public(from_value, property.key)
                : property.value;
            to->set(
                property.key,
                clone_between(source, target, property_value, seen));
        }
    };

    switch (object->kind) {
        case HeapObject::kJSArray: {
            auto* from = static_cast<JSArray*>(object);
            JSArray* to = target.new_array();
            Value copy = Value::make_heap_ptr(to);
            seen.emplace_back(object, copy);
            for (size_t i = 0; i < from->elements.size(); ++i) {
                const bool present = from->has_index(i);
                to->append(
                    present
                        ? clone_between(
                              source, target, from->elements[i], seen)
                        : Value::make_undefined(),
                    present);
            }
            clone_properties(from, to, value);
            return copy;
        }
        case HeapObject::kJSObject: {
            auto* from = static_cast<JSObject*>(object);
            JSObject* to = target.new_object();
            Value copy = Value::make_heap_ptr(to);
            seen.emplace_back(object, copy);
            clone_properties(from, to, value);
            return copy;
        }
        case HeapObject::kJSMap: {
            auto* from = static_cast<JSMap*>(object);
            JSMap* to = target.new_map();
            Value copy = Value::make_heap_ptr(to);
            seen.emplace_back(object, copy);
            for (const auto& [key, entry] : from->entries) {
                to->entries.emplace_back(
                    clone_between(source, target, key, seen),
                    clone_between(source, target, entry, seen));
            }
            clone_properties(from, to, value);
            return copy;
        }
        case HeapObject::kJSSet: {
            auto* from = static_cast<JSSet*>(object);
            JSSet* to = target.new_set();
            Value copy = Value::make_heap_ptr(to);
            seen.emplace_back(object, copy);
            for (Value entry : from->items)
                to->items.push_back(
                    clone_between(source, target, entry, seen));
            clone_properties(from, to, value);
            return copy;
        }
        case HeapObject::kArrayBuffer: {
            auto* from = static_cast<JSArrayBuffer*>(object);
            JSArrayBuffer* to =
                target.new_array_buffer(from->detached
                                            ? std::vector<uint8_t>()
                                            : from->data);
            to->detached = from->detached;
            Value copy = Value::make_heap_ptr(to);
            seen.emplace_back(object, copy);
            clone_properties(from, to, value);
            return copy;
        }
        case HeapObject::kTypedArray: {
            auto* from = static_cast<JSTypedArray*>(object);
            static constexpr const char16_t* names[] = {
                u"Int8Array", u"Uint8Array", u"Uint8ClampedArray",
                u"Int16Array", u"Uint16Array", u"Int32Array",
                u"Uint32Array", u"Float32Array", u"Float64Array",
            };
            JSArrayBuffer* buffer = target.new_array_buffer(
                from->buffer ? from->buffer->data : std::vector<uint8_t>());
            Value* constructor = target.global()->find(
                names[static_cast<size_t>(from->ta_kind)]);
            if (!constructor)
                source.throw_error(
                    u"DataCloneError",
                    u"Typed array constructor is unavailable");
            std::vector<Value> arguments{
                Value::make_heap_ptr(buffer),
                Value::make_int32(static_cast<int32_t>(from->byte_offset)),
                Value::make_int32(static_cast<int32_t>(from->length)),
            };
            Value copy = target.construct(*constructor, arguments);
            seen.emplace_back(object, copy);
            if (copy.is_heap_ptr())
                clone_properties(
                    from, static_cast<JSObject*>(copy.as_heap_ptr()), value);
            return copy;
        }
        case HeapObject::kDataView: {
            auto* from = static_cast<JSDataView*>(object);
            JSArrayBuffer* buffer = target.new_array_buffer(
                from->buffer ? from->buffer->data : std::vector<uint8_t>());
            Value* constructor = target.global()->find(u"DataView");
            if (!constructor)
                source.throw_error(
                    u"DataCloneError",
                    u"DataView constructor is unavailable");
            std::vector<Value> arguments{
                Value::make_heap_ptr(buffer),
                Value::make_int32(static_cast<int32_t>(from->byte_offset)),
                Value::make_int32(static_cast<int32_t>(from->byte_length)),
            };
            Value copy = target.construct(*constructor, arguments);
            seen.emplace_back(object, copy);
            if (copy.is_heap_ptr())
                clone_properties(
                    from, static_cast<JSObject*>(copy.as_heap_ptr()), value);
            return copy;
        }
        default:
            source.throw_error(
                u"DataCloneError",
                u"Value cannot be cloned for postMessage");
    }
}

Value clone_between(
    Interpreter& source, Interpreter& target, Value value) {
    std::vector<std::pair<HeapObject*, Value>> seen;
    return clone_between(source, target, value, seen);
}

Value make_storage_object(
    Interpreter& interpreter,
    std::function<storage::Storage&()> get_storage) {
    JSObject* object = interpreter.new_object();
    auto widen_ascii = [](const std::string& value) {
        return std::u16string(value.begin(), value.end());
    };
    auto sync_length = [get_storage, object] {
        object->set(
            u"length",
            Value::make_int32(
                static_cast<int32_t>(get_storage().length())));
    };
    sync_length();
    object->set(
        u"getItem",
        Value::make_heap_ptr(interpreter.new_native(
            u"getItem",
            [get_storage, widen_ascii](
                Interpreter& in, Value,
                std::vector<Value>& arguments) -> Value {
                const Value key = arguments.empty()
                    ? Value::make_undefined()
                    : arguments[0];
                auto value = get_storage().get_item(
                    narrow(in.to_string(key)));
                return value
                    ? in.str(widen_ascii(*value))
                    : Value::make_null();
            },
            1)));
    object->set(
        u"setItem",
        Value::make_heap_ptr(interpreter.new_native(
            u"setItem",
            [get_storage, sync_length](
                Interpreter& in, Value,
                std::vector<Value>& arguments) -> Value {
                if (arguments.size() >= 2) {
                    get_storage().set_item(
                        narrow(in.to_string(arguments[0])),
                        narrow(in.to_string(arguments[1])));
                }
                sync_length();
                return Value::make_undefined();
            },
            2)));
    object->set(
        u"removeItem",
        Value::make_heap_ptr(interpreter.new_native(
            u"removeItem",
            [get_storage, sync_length](
                Interpreter& in, Value,
                std::vector<Value>& arguments) -> Value {
                if (!arguments.empty()) {
                    get_storage().remove_item(
                        narrow(in.to_string(arguments[0])));
                }
                sync_length();
                return Value::make_undefined();
            },
            1)));
    object->set(
        u"clear",
        Value::make_heap_ptr(interpreter.new_native(
            u"clear",
            [get_storage, sync_length](
                Interpreter&, Value,
                std::vector<Value>&) -> Value {
                get_storage().clear();
                sync_length();
                return Value::make_undefined();
            })));
    object->set(
        u"key",
        Value::make_heap_ptr(interpreter.new_native(
            u"key",
            [get_storage, widen_ascii](
                Interpreter& in, Value,
                std::vector<Value>& arguments) -> Value {
                const size_t index = arguments.empty()
                    ? 0
                    : static_cast<size_t>(
                          in.to_number(arguments[0]));
                auto key = get_storage().key(index);
                return key
                    ? in.str(widen_ascii(*key))
                    : Value::make_null();
            },
            1)));
    return Value::make_heap_ptr(object);
}

void install_event_target(Interpreter& interpreter, JSObject* target) {
    JSObject* listeners = interpreter.new_object();
    target->set(
        u"%listeners%", Value::make_heap_ptr(listeners), false);
    target->set(
        u"addEventListener",
        Value::make_heap_ptr(interpreter.new_native(
            u"addEventListener",
            [listeners](Interpreter& in, Value,
                        std::vector<Value>& arguments) -> Value {
                if (arguments.size() < 2 ||
                    !in.is_callable(arguments[1]))
                    return Value::make_undefined();
                const std::u16string type =
                    in.to_string(arguments[0]);
                Value callbacks_value = listeners->get(type);
                JSArray* callbacks = nullptr;
                if (callbacks_value.is_heap_ptr() &&
                    callbacks_value.as_heap_ptr()->kind ==
                        HeapObject::kJSArray) {
                    callbacks =
                        static_cast<JSArray*>(
                            callbacks_value.as_heap_ptr());
                } else {
                    callbacks = in.new_array();
                    listeners->set(
                        type, Value::make_heap_ptr(callbacks));
                }
                callbacks->elements.push_back(arguments[1]);
                return Value::make_undefined();
            },
            2)));
    target->set(
        u"removeEventListener",
        Value::make_heap_ptr(interpreter.new_native(
            u"removeEventListener",
            [listeners](Interpreter& in, Value,
                        std::vector<Value>& arguments) -> Value {
                if (arguments.size() < 2)
                    return Value::make_undefined();
                Value callbacks_value =
                    listeners->get(in.to_string(arguments[0]));
                if (callbacks_value.is_heap_ptr() &&
                    callbacks_value.as_heap_ptr()->kind ==
                        HeapObject::kJSArray) {
                    auto& callbacks =
                        static_cast<JSArray*>(
                            callbacks_value.as_heap_ptr())
                            ->elements;
                    callbacks.erase(
                        std::remove(
                            callbacks.begin(), callbacks.end(),
                            arguments[1]),
                        callbacks.end());
                }
                return Value::make_undefined();
            },
            2)));
}

void dispatch_event_target(
    Interpreter& interpreter,
    Value target_value,
    const std::u16string& type,
    Value data,
    const std::u16string& message = u"") {
    if (!target_value.is_heap_ptr() ||
        target_value.as_heap_ptr()->kind != HeapObject::kJSObject)
        return;
    JSObject* event = interpreter.new_object();
    Value event_value = Value::make_heap_ptr(event);
    event->set(u"type", interpreter.str(type));
    event->set(u"data", data);
    event->set(u"message", interpreter.str(message));
    event->set(u"target", target_value);
    event->set(u"currentTarget", target_value);

    std::vector<Value> callbacks;
    const std::u16string handler_name = u"on" + type;
    Value handler = interpreter.get_prop_public(
        target_value, handler_name);
    if (interpreter.is_callable(handler))
        callbacks.push_back(handler);
    Value listeners_value =
        interpreter.get_prop_public(target_value, u"%listeners%");
    if (listeners_value.is_heap_ptr() &&
        listeners_value.as_heap_ptr()->kind == HeapObject::kJSObject) {
        Value array_value = interpreter.get_prop_public(
            listeners_value, type);
        if (array_value.is_heap_ptr() &&
            array_value.as_heap_ptr()->kind == HeapObject::kJSArray) {
            const auto& registered =
                static_cast<JSArray*>(array_value.as_heap_ptr())
                    ->elements;
            callbacks.insert(
                callbacks.end(), registered.begin(), registered.end());
        }
    }

    interpreter.push_root(event_value);
    for (Value callback : callbacks) {
        try {
            std::vector<Value> arguments{event_value};
            interpreter.call(callback, target_value, arguments);
        } catch (ThrowSignal&) {
            // An event-listener exception is reported but does not stop the
            // remaining listeners or the worker's event loop.
        }
    }
    interpreter.pop_root();
    interpreter.run_microtasks();
}

void install_dom_exception(Interpreter& interpreter) {
    JSObject* prototype = interpreter.new_object();
    const std::initializer_list<
        std::tuple<const char16_t*, int32_t, const char16_t*>>
        legacy_codes = {
            {u"INDEX_SIZE_ERR", 1, u"IndexSizeError"},
            {u"DOMSTRING_SIZE_ERR", 2, u"DOMStringSizeError"},
            {u"HIERARCHY_REQUEST_ERR", 3, u"HierarchyRequestError"},
            {u"WRONG_DOCUMENT_ERR", 4, u"WrongDocumentError"},
            {u"INVALID_CHARACTER_ERR", 5, u"InvalidCharacterError"},
            {u"NO_DATA_ALLOWED_ERR", 6, u"NoDataAllowedError"},
            {u"NO_MODIFICATION_ALLOWED_ERR", 7,
             u"NoModificationAllowedError"},
            {u"NOT_FOUND_ERR", 8, u"NotFoundError"},
            {u"NOT_SUPPORTED_ERR", 9, u"NotSupportedError"},
            {u"INUSE_ATTRIBUTE_ERR", 10, u"InUseAttributeError"},
            {u"INVALID_STATE_ERR", 11, u"InvalidStateError"},
            {u"SYNTAX_ERR", 12, u"SyntaxError"},
            {u"INVALID_MODIFICATION_ERR", 13,
             u"InvalidModificationError"},
            {u"NAMESPACE_ERR", 14, u"NamespaceError"},
            {u"INVALID_ACCESS_ERR", 15, u"InvalidAccessError"},
            {u"VALIDATION_ERR", 16, u"ValidationError"},
            {u"TYPE_MISMATCH_ERR", 17, u"TypeMismatchError"},
            {u"SECURITY_ERR", 18, u"SecurityError"},
            {u"NETWORK_ERR", 19, u"NetworkError"},
            {u"ABORT_ERR", 20, u"AbortError"},
            {u"URL_MISMATCH_ERR", 21, u"URLMismatchError"},
            {u"QUOTA_EXCEEDED_ERR", 22, u"QuotaExceededError"},
            {u"TIMEOUT_ERR", 23, u"TimeoutError"},
            {u"INVALID_NODE_TYPE_ERR", 24, u"InvalidNodeTypeError"},
            {u"DATA_CLONE_ERR", 25, u"DataCloneError"},
        };
    JSFunction* constructor = interpreter.new_native(
        u"DOMException",
        [prototype, legacy_codes](
            Interpreter& in, Value,
            std::vector<Value>& arguments) -> Value {
            const std::u16string message =
                arguments.empty()
                    ? u""
                    : in.to_string(arguments[0]);
            const std::u16string name =
                arguments.size() < 2
                    ? u"Error"
                    : in.to_string(arguments[1]);
            int32_t code = 0;
            for (const auto& [constant, value, error_name] :
                 legacy_codes) {
                (void)constant;
                if (name == error_name) {
                    code = value;
                    break;
                }
            }
            JSObject* exception = in.new_object();
            exception->proto = prototype;
            exception->set(u"name", in.str(name), false);
            exception->set(
                u"message", in.str(message), false);
            exception->set(
                u"code", Value::make_int32(code), false);
            std::u16string stack = name;
            if (!message.empty()) stack += u": " + message;
            exception->set(
                u"stack", in.str(stack), false);
            return Value::make_heap_ptr(exception);
        },
        0);
    constructor->set(
        u"prototype", Value::make_heap_ptr(prototype), false);
    prototype->set(
        u"constructor", Value::make_heap_ptr(constructor), false);
    prototype->set(u"name", interpreter.str("Error"), false);
    prototype->set(u"message", interpreter.str(""), false);
    prototype->set(u"code", Value::make_int32(0), false);
    prototype->set(
        u"toString",
        Value::make_heap_ptr(interpreter.new_native(
            u"toString",
            [](Interpreter& in, Value receiver,
               std::vector<Value>&) -> Value {
                std::u16string name = in.to_string(
                    in.get_prop_public(receiver, u"name"));
                std::u16string message = in.to_string(
                    in.get_prop_public(receiver, u"message"));
                if (name.empty()) return in.str(message);
                if (message.empty()) return in.str(name);
                return in.str(name + u": " + message);
            })));
    for (const auto& [name, value, error_name] : legacy_codes) {
        (void)error_name;
        constructor->set(name, Value::make_int32(value), false);
        prototype->set(name, Value::make_int32(value), false);
    }
    interpreter.global()->define(
        u"DOMException", Value::make_heap_ptr(constructor));
}
}  // namespace

struct View::WorkerState {
    js::Engine engine;
    Value main_object = Value::make_undefined();
    std::string url;
    bool terminated = false;
};

View::View() {
    origin_ = security::Origin::parse("about:blank");
    if (fonts_.available()) {
        measurer_ = std::make_unique<text::FreeTypeTextMeasurer>(fonts_);
        drawer_ = std::make_unique<text::FreeTypeTextDrawer>(fonts_);
        layout_.set_text_measurer(measurer_.get());  // real font metrics in layout
    }
    reset_document();
}

View::~View() = default;

bool View::perform_request(
    const network::FetchRequest& request,
    network::FetchResponse& response) {
    if (sandbox_ & SandboxNoNetwork) return false;
    if (fetch_handler_) return fetch_handler_(request, response);
    return request_handler_ &&
           request_handler_(request.url, response);
}

bool View::perform_request(
    const std::string& url,
    network::FetchResponse& response) {
    network::FetchRequest request;
    request.url = url;
    request.referrer = current_url_;
    return perform_request(request, response);
}

bool View::load_script_source(
    const std::string& url, std::string& source) {
    if (url.rfind("data:", 0) == 0)
        return decode_data_url(url, source);
    network::FetchResponse response;
    if (!perform_request(url, response) || is_http_error(response))
        return false;
    source.assign(response.body.begin(), response.body.end());
    return true;
}

void View::reset_document() {
    for (auto& worker : workers_) {
        worker->terminated = true;
        worker->engine.event_loop().quit();
        engine_.interpreter().remove_host_root(worker->main_object);
    }
    workers_.clear();
    for (const auto& [id, socket] : sockets_) {
        if (socket_handler_)
            socket_handler_(id, "", "1001\nDocument navigated", 2);
        engine_.interpreter().remove_host_root(socket);
    }
    sockets_.clear();
    for (js::runtime::Value root : wasm_host_roots_)
        engine_.interpreter().remove_host_root(root);
    wasm_host_roots_.clear();
    wasm_reference_values_.clear();
    wasm_instances_.clear();
    wasm_modules_.clear();
    engine_.interpreter().clear_dom_wrappers();   // old document's node wrappers are stale
    doc_ = std::make_unique<dom::Document>();
    tree_ = std::make_unique<dom::DOMTree>(*doc_);
    resolver_ = std::make_unique<css::StyleResolver>();
    binding_ = std::make_unique<js::DomBinding>(engine_.interpreter(), *tree_, doc_->root());
    binding_->install();
    binding_->set_context_provider([this](malibu::NodeHandle node, const std::u16string& type) {
        return make_canvas_context(node, type);
    });
    // getBoundingClientRect / offset* / client*: lay out on demand (scripts run
    // before the first render) and read the element's layout box.
    binding_->set_rect_provider([this](malibu::NodeHandle n, float& x, float& y, float& w, float& h) -> bool {
        if (layout_dirty_ || !layout_.root()) {
            apply_styles();
            layout_.layout_document(*doc_, static_cast<float>(last_vw_),
                                    static_cast<float>(last_vh_));
            layout_dirty_ = false;
        }
        layout::LayoutBox* b = layout_.box_for_node(n);
        if (!b) return false;
        x = b->x; y = b->y; w = b->width; h = b->height; return true;
    });
    binding_->set_mutation_handler(
        [this](malibu::NodeHandle node) { handle_dom_mutation(node); });
    binding_->set_cookie_handlers(
        [this] {
            return widen(storage_.cookies().get_cookie_string(current_url_));
        },
        [this](const std::u16string& assignment) {
            storage_.cookies().set_cookie_from_document(
                current_url_, narrow(assignment));
        });
    binding_->set_submit_handler(
        [this](malibu::NodeHandle form) { handle_form_submit(form); });
    canvases_.clear();
    prepared_resource_nodes_.clear();
    layout_.clear_replaced_intrinsic_sizes();
    replaced_content_dirty_ = true;
    install_view_globals();
}

void View::handle_form_submit(malibu::NodeHandle form) {
    const dom::NodeCore* form_core = doc_->core(form);
    if (!form_core || form_core->tag_name != u"form") return;

    std::string method = trim_ascii_lower(
        tree_->get_attribute(form, u"method").value_or(u"get"));
    if (method.empty()) method = "get";
    if (method != "get") {
        record_diagnostic(
            LoadDiagnosticKind::Unsupported, current_url_,
            "form submission method '" + method + "' is not implemented");
        return;
    }

    std::vector<std::pair<std::u16string, std::u16string>> entries;
    std::function<void(malibu::NodeHandle)> collect =
        [&](malibu::NodeHandle parent) {
            const dom::NodeCore* parent_core = doc_->core(parent);
            if (!parent_core) return;
            for (malibu::NodeHandle child : parent_core->children) {
                const dom::NodeCore* core = doc_->core(child);
                if (!core) continue;
                if (core->node_type == dom::kElementNode) {
                    const auto name = tree_->get_attribute(child, u"name");
                    const bool disabled =
                        tree_->get_attribute(child, u"disabled").has_value();
                    if (name && !name->empty() && !disabled) {
                        if (core->tag_name == u"input") {
                            std::string type = trim_ascii_lower(
                                tree_->get_attribute(child, u"type")
                                    .value_or(u"text"));
                            const bool checkable =
                                type == "checkbox" || type == "radio";
                            const bool successful =
                                type != "button" && type != "file" &&
                                type != "image" && type != "reset" &&
                                type != "submit" &&
                                (!checkable ||
                                 tree_->get_attribute(child, u"checked")
                                     .has_value());
                            if (successful) {
                                std::u16string value =
                                    tree_->get_attribute(child, u"value")
                                        .value_or(checkable ? u"on" : u"");
                                entries.emplace_back(*name, std::move(value));
                            }
                        } else if (core->tag_name == u"textarea") {
                            entries.emplace_back(
                                *name,
                                tree_->get_attribute(child, u"value")
                                    .value_or(tree_->text_content(child)));
                        } else if (core->tag_name == u"select") {
                            bool selected_any = false;
                            std::function<void(malibu::NodeHandle)> options =
                                [&](malibu::NodeHandle option_parent) {
                                    const dom::NodeCore* option_parent_core =
                                        doc_->core(option_parent);
                                    if (!option_parent_core) return;
                                    for (malibu::NodeHandle option :
                                         option_parent_core->children) {
                                        const dom::NodeCore* option_core =
                                            doc_->core(option);
                                        if (!option_core) continue;
                                        if (option_core->tag_name == u"option" &&
                                            tree_->get_attribute(
                                                option, u"selected")
                                                .has_value()) {
                                            entries.emplace_back(
                                                *name,
                                                tree_->get_attribute(
                                                    option, u"value")
                                                    .value_or(
                                                        tree_->text_content(
                                                            option)));
                                            selected_any = true;
                                        }
                                        options(option);
                                    }
                                };
                            options(child);
                            if (!selected_any) {
                                malibu::NodeHandle option =
                                    tree_->query_selector(child, u"option");
                                if (!option.is_null())
                                    entries.emplace_back(
                                        *name,
                                        tree_->get_attribute(option, u"value")
                                            .value_or(
                                                tree_->text_content(option)));
                            }
                        }
                    }
                }
                collect(child);
            }
        };
    collect(form);

    std::string query;
    for (const auto& [name, value] : entries) {
        if (!query.empty()) query.push_back('&');
        query += form_urlencode(name);
        query.push_back('=');
        query += form_urlencode(value);
    }

    const std::u16string action =
        tree_->get_attribute(form, u"action").value_or(u"");
    std::string target = action.empty() ? current_url_ : resolve_url(action);
    std::string fragment;
    if (const size_t hash = target.find('#');
        hash != std::string::npos) {
        fragment = target.substr(hash);
        target.erase(hash);
    }
    if (const size_t question = target.find('?');
        question != std::string::npos)
        target.erase(question);
    if (!query.empty()) target += "?" + query;
    pending_navigation_url_ = target + fragment;
}

void View::process_pending_navigation() {
    if (processing_pending_navigation_) return;
    processing_pending_navigation_ = true;
    for (size_t redirects = 0;
         redirects < 10 && !pending_navigation_url_.empty();
         ++redirects) {
        std::string target = std::move(pending_navigation_url_);
        pending_navigation_url_.clear();
        if (!load_url(target)) break;
    }
    processing_pending_navigation_ = false;
}

void View::install_worker_globals() {
    auto& main = engine_.interpreter();
    JSObject* worker_proto = main.new_object();
    View* owner = this;

    JSFunction* worker_ctor = main.new_native(
        u"Worker",
        [owner, worker_proto](
            Interpreter& in, Value,
            std::vector<Value>& arguments) -> Value {
            if (arguments.empty())
                in.throw_error(
                    u"TypeError",
                    u"Failed to construct 'Worker': 1 argument required");

            Value href = arguments[0].is_heap_ptr()
                ? in.get_prop_public(arguments[0], u"href")
                : Value::make_undefined();
            std::string requested = narrow(in.to_string(
                href.is_undefined() ? arguments[0] : href));
            std::string url =
                resolve_against(owner->current_url_, requested);

            std::u16string name;
            std::u16string type = u"classic";
            if (arguments.size() > 1 &&
                arguments[1].is_heap_ptr()) {
                Value name_value =
                    in.get_prop_public(arguments[1], u"name");
                if (!name_value.is_undefined())
                    name = in.to_string(name_value);
                Value type_value =
                    in.get_prop_public(arguments[1], u"type");
                if (!type_value.is_undefined())
                    type = in.to_string(type_value);
            }

            JSObject* worker_object = in.new_object();
            worker_object->proto = worker_proto;
            install_event_target(in, worker_object);
            worker_object->set(
                u"onmessage", Value::make_null());
            worker_object->set(
                u"onmessageerror", Value::make_null());
            worker_object->set(
                u"onerror", Value::make_null());
            Value worker_value =
                Value::make_heap_ptr(worker_object);

            auto state = std::make_shared<WorkerState>();
            state->main_object = worker_value;
            state->url = url;
            state->engine.interpreter().set_console_sink(
                in.console_sink());
            owner->workers_.push_back(state);
            in.add_host_root(worker_value);

            Interpreter& worker = state->engine.interpreter();
            JSObject* worker_global =
                worker.global()->object_backing;
            Value worker_global_value =
                Value::make_heap_ptr(worker_global);
            install_event_target(worker, worker_global);
            worker_global->set(u"self", worker_global_value);
            worker_global->set(
                u"name", worker.str(name));
            worker_global->set(
                u"onmessage", Value::make_null());
            worker_global->set(
                u"onmessageerror", Value::make_null());
            worker_global->set(
                u"onerror", Value::make_null());

            JSObject* location = worker.new_object();
            location->set(u"href", worker.str(url));
            const size_t scheme = url.find("://");
            const size_t path = scheme == std::string::npos
                ? std::string::npos
                : url.find('/', scheme + 3);
            const std::string origin =
                scheme == std::string::npos
                    ? std::string()
                    : (path == std::string::npos
                           ? url
                           : url.substr(0, path));
            location->set(u"origin", worker.str(origin));
            location->set(
                u"toString",
                Value::make_heap_ptr(worker.new_native(
                    u"toString",
                    [location](
                        Interpreter&, Value,
                        std::vector<Value>&) -> Value {
                        return location->get(u"href");
                    })));
            worker_global->set(
                u"location", Value::make_heap_ptr(location));

            JSObject* navigator = worker.new_object();
            navigator->set(
                u"userAgent",
                worker.str(
                    "Mozilla/5.0 (X11; Linux x86_64) "
                    "AppleWebKit/537.36 Malibu/1.0"));
            navigator->set(
                u"hardwareConcurrency", Value::make_int32(4));
            navigator->set(
                u"language", worker.str("en-US"));
            navigator->set(u"onLine", Value::make_bool(true));
            worker_global->set(
                u"navigator", Value::make_heap_ptr(navigator));
            install_dom_exception(worker);

            std::weak_ptr<WorkerState> weak_state = state;
            worker_object->set(
                u"postMessage",
                Value::make_heap_ptr(in.new_native(
                    u"postMessage",
                    [owner, weak_state](
                        Interpreter& source, Value,
                        std::vector<Value>& args) -> Value {
                        auto live = weak_state.lock();
                        if (!live || live->terminated)
                            return Value::make_undefined();
                        Interpreter& target =
                            live->engine.interpreter();
                        Value data = clone_between(
                            source, target,
                            args.empty()
                                ? Value::make_undefined()
                                : args[0]);
                        target.add_host_root(data);
                        live->engine.event_loop().post_task(
                            [live, data] {
                                Interpreter& in =
                                    live->engine.interpreter();
                                if (!live->terminated) {
                                    dispatch_event_target(
                                        in,
                                        Value::make_heap_ptr(
                                            in.global()
                                                ->object_backing),
                                        u"message", data);
                                }
                                in.remove_host_root(data);
                            });
                        return Value::make_undefined();
                    },
                    1)));
            worker_object->set(
                u"terminate",
                Value::make_heap_ptr(in.new_native(
                    u"terminate",
                    [weak_state](
                        Interpreter&, Value,
                        std::vector<Value>&) -> Value {
                        if (auto live = weak_state.lock()) {
                            live->terminated = true;
                            live->engine.event_loop().quit();
                        }
                        return Value::make_undefined();
                    })));

            worker_global->set(
                u"postMessage",
                Value::make_heap_ptr(worker.new_native(
                    u"postMessage",
                    [owner, weak_state](
                        Interpreter& source, Value,
                        std::vector<Value>& args) -> Value {
                        auto live = weak_state.lock();
                        if (!live || live->terminated)
                            return Value::make_undefined();
                        Interpreter& target =
                            owner->engine_.interpreter();
                        Value data = clone_between(
                            source, target,
                            args.empty()
                                ? Value::make_undefined()
                                : args[0]);
                        target.add_host_root(data);
                        owner->engine_.event_loop().post_task(
                            [live, owner, data] {
                                Interpreter& in =
                                    owner->engine_.interpreter();
                                if (!live->terminated)
                                    dispatch_event_target(
                                        in, live->main_object,
                                        u"message", data);
                                in.remove_host_root(data);
                            });
                        return Value::make_undefined();
                    },
                    1)));
            worker_global->set(
                u"close",
                Value::make_heap_ptr(worker.new_native(
                    u"close",
                    [weak_state](
                        Interpreter&, Value,
                        std::vector<Value>&) -> Value {
                        if (auto live = weak_state.lock()) {
                            live->terminated = true;
                            live->engine.event_loop().quit();
                        }
                        return Value::make_undefined();
                    })));
            worker_global->set(
                u"importScripts",
                Value::make_heap_ptr(worker.new_native(
                    u"importScripts",
                    [owner, weak_state](
                        Interpreter& worker_in, Value,
                        std::vector<Value>& args) -> Value {
                        auto live = weak_state.lock();
                        if (!live || live->terminated)
                            return Value::make_undefined();
                        for (Value argument : args) {
                            const std::string script_url =
                                resolve_against(
                                    live->url,
                                    narrow(worker_in.to_string(
                                        argument)));
                            network::FetchResponse response;
                            if (!owner->perform_request(
                                    script_url, response) ||
                                is_http_error(response)) {
                                worker_in.throw_error(
                                    u"NetworkError",
                                    u"Failed to load worker script");
                            }
                            auto result = live->engine.evaluate(
                                std::string(
                                    response.body.begin(),
                                    response.body.end()),
                                response.url.empty()
                                    ? script_url
                                    : response.url);
                            if (!result.ok)
                                worker_in.throw_error(
                                    u"Error",
                                    widen(result.error));
                        }
                        return Value::make_undefined();
                    })));

            JSFunction* worker_scope = worker.new_native(
                u"WorkerGlobalScope",
                [](Interpreter& worker_in, Value,
                   std::vector<Value>&) -> Value {
                    worker_in.throw_error(
                        u"TypeError", u"Illegal constructor");
                });
            JSObject* worker_scope_proto =
                worker.new_object();
            worker_scope->set(
                u"prototype",
                Value::make_heap_ptr(worker_scope_proto), false);
            worker_global->set(
                u"WorkerGlobalScope",
                Value::make_heap_ptr(worker_scope));
            worker_global->set(
                u"DedicatedWorkerGlobalScope",
                Value::make_heap_ptr(worker_scope));

            if (type == u"module") {
                owner->record_diagnostic(
                    LoadDiagnosticKind::Unsupported, url,
                    "module workers are not implemented");
                owner->engine_.event_loop().post_task(
                    [state, owner] {
                        if (!state->terminated)
                            dispatch_event_target(
                                owner->engine_.interpreter(),
                                state->main_object, u"error",
                                Value::make_undefined(),
                                u"Module workers are not implemented");
                    });
                return worker_value;
            }

            network::FetchResponse response;
            if (!owner->perform_request(url, response) ||
                is_http_error(response)) {
                owner->record_diagnostic(
                    LoadDiagnosticKind::Resource, url,
                    "failed to fetch worker script");
                owner->engine_.event_loop().post_task(
                    [state, owner] {
                        if (!state->terminated)
                            dispatch_event_target(
                                owner->engine_.interpreter(),
                                state->main_object, u"error",
                                Value::make_undefined(),
                                u"Failed to load worker script");
                    });
                return worker_value;
            }

            const std::string final_url =
                response.url.empty() ? url : response.url;
            state->url = final_url;
            location->set(u"href", worker.str(final_url));
            auto result = state->engine.evaluate(
                std::string(
                    response.body.begin(), response.body.end()),
                final_url);
            if (!result.ok) {
                owner->record_diagnostic(
                    LoadDiagnosticKind::Script, final_url,
                    result.error);
                const std::u16string error = widen(result.error);
                owner->engine_.event_loop().post_task(
                    [state, owner, error] {
                        if (!state->terminated)
                            dispatch_event_target(
                                owner->engine_.interpreter(),
                                state->main_object, u"error",
                                Value::make_undefined(), error);
                    });
            }
            return worker_value;
        },
        1);
    worker_ctor->set(
        u"prototype", Value::make_heap_ptr(worker_proto), false);
    worker_proto->set(
        u"constructor", Value::make_heap_ptr(worker_ctor), false);
    main.global()->define(
        u"Worker", Value::make_heap_ptr(worker_ctor));
}

void View::install_view_globals() {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArray;
    using js::runtime::JSPromise;
    using js::runtime::JSFunction;
    using js::runtime::JSArrayBuffer;
    using js::runtime::JSTypedArray;
    auto& interp = engine_.interpreter();
    View* self = this;
    install_dom_exception(interp);

    // ---- HTMLAudioElement constructor ------------------------------------
    // The DOM binding owns the media element state. Audio() only creates the
    // same native DOM node that document.createElement("audio") would return.
    JSFunction* audio_ctor = interp.new_native(
        u"Audio",
        [self](Interpreter& in, Value,
               std::vector<Value>& arguments) -> Value {
            malibu::NodeHandle audio = self->tree_->create_element(u"audio");
            self->tree_->set_attribute(audio, u"preload", u"auto");
            if (!arguments.empty())
                self->tree_->set_attribute(
                    audio, u"src", in.to_string(arguments[0]));
            return in.make_dom_node(audio);
        },
        0);
    JSObject* audio_proto = interp.new_object();
    audio_ctor->set(
        u"prototype", Value::make_heap_ptr(audio_proto), false);
    audio_ctor->set(
        u"__domInterface", interp.str("HTMLAudioElement"), false);
    audio_proto->set(
        u"constructor", Value::make_heap_ptr(audio_ctor), false);
    interp.global()->define(u"Audio", Value::make_heap_ptr(audio_ctor));

    // ---- HTMLImageElement constructor ------------------------------------
    // `new Image(width, height)` is equivalent to creating an <img> element
    // and then assigning its reflected dimensions.
    JSFunction* image_ctor = interp.new_native(
        u"Image",
        [self](Interpreter& in, Value,
               std::vector<Value>& arguments) -> Value {
            malibu::NodeHandle image = self->tree_->create_element(u"img");
            if (!arguments.empty() && !arguments[0].is_undefined())
                self->tree_->set_attribute(
                    image, u"width", in.to_string(arguments[0]));
            if (arguments.size() > 1 && !arguments[1].is_undefined())
                self->tree_->set_attribute(
                    image, u"height", in.to_string(arguments[1]));
            return in.make_dom_node(image);
        },
        0);
    JSObject* image_proto = interp.new_object();
    image_ctor->set(
        u"prototype", Value::make_heap_ptr(image_proto), false);
    image_ctor->set(
        u"__domInterface", interp.str("HTMLImageElement"), false);
    image_proto->set(
        u"constructor", Value::make_heap_ptr(image_ctor), false);
    interp.global()->define(u"Image", Value::make_heap_ptr(image_ctor));

    // ---- DOMParser -------------------------------------------------------
    // Parsed documents live as detached document fragments in the same native
    // node arena. The binding exposes documentElement/head/body and all
    // ParentNode query APIs for detached fragments.
    JSObject* dom_parser_proto = interp.new_object();
    JSFunction* dom_parser_ctor = interp.new_native(
        u"DOMParser",
        [self, dom_parser_proto](Interpreter& in, Value,
                                 std::vector<Value>&) -> Value {
            JSObject* parser = in.new_object();
            parser->proto = dom_parser_proto;
            parser->set(
                u"parseFromString",
                Value::make_heap_ptr(in.new_native(
                    u"parseFromString",
                    [self](Interpreter& in2, Value,
                           std::vector<Value>& arguments) -> Value {
                        const std::u16string source =
                            arguments.empty()
                                ? std::u16string()
                                : in2.to_string(arguments[0]);
                        malibu::NodeHandle document =
                            self->tree_->create_document_fragment();
                        malibu::html::HTMLParser().parse_fragment(
                            source, *self->tree_, document);
                        return in2.make_dom_node(document);
                    },
                    2)));
            return Value::make_heap_ptr(parser);
        },
        0);
    dom_parser_ctor->set(
        u"prototype", Value::make_heap_ptr(dom_parser_proto), false);
    dom_parser_proto->set(
        u"constructor", Value::make_heap_ptr(dom_parser_ctor), false);
    interp.global()->define(
        u"DOMParser", Value::make_heap_ptr(dom_parser_ctor));

    // Dynamic import uses the same URL and resource-loading path as script
    // elements. Module namespace linking is intentionally minimal for now,
    // but evaluation, side effects, promise timing and failures are real.
    interp.global()->define(
        u"__dynamicImport",
        Value::make_heap_ptr(interp.new_native(
            u"__dynamicImport",
            [self](Interpreter& in, Value,
                   std::vector<Value>& arguments) -> Value {
                JSPromise* promise = in.new_promise();
                if (arguments.empty()) {
                    in.reject_promise(
                        promise,
                        in.str("TypeError: import() requires a module specifier"));
                    return Value::make_heap_ptr(promise);
                }
                const std::string url =
                    self->resolve_url(in.to_string(arguments[0]));
                std::string source;
                if (!self->load_script_source(url, source)) {
                    in.reject_promise(
                        promise,
                        in.str(std::string("TypeError: Failed to load module: ") +
                               url));
                    return Value::make_heap_ptr(promise);
                }
                set_import_meta(self->engine_, url);
                js::Engine::EvalResult result =
                    self->engine_.evaluate_module(source, url);
                if (!result.ok) {
                    self->record_diagnostic(
                        LoadDiagnosticKind::Script, url, result.error);
                    in.reject_promise(promise, in.str(result.error));
                    return Value::make_heap_ptr(promise);
                }
                in.resolve_promise(
                    promise, Value::make_heap_ptr(in.new_object()));
                return Value::make_heap_ptr(promise);
            },
            1)));

    // JS -> native: window.malibuNativeMessage("...")
    auto* fn = interp.new_native(u"malibuNativeMessage",
        [self](Interpreter& in, Value, std::vector<Value>& a) {
            if (self->message_handler_ && !a.empty())
                self->message_handler_(narrow(in.to_string(a[0])));
            return Value::make_undefined();
        });
    interp.global()->define(u"malibuNativeMessage", Value::make_heap_ptr(fn));

    // ---- WebSocket (WHATWG surface, transport supplied by the host) ----
    JSObject* websocket_proto = interp.new_object();
    JSFunction* websocket_ctor = interp.new_native(
        u"WebSocket",
        [self, websocket_proto](Interpreter& in, Value,
                                std::vector<Value>& a) -> Value {
            if (a.empty())
                in.throw_error(u"TypeError",
                               u"Failed to construct 'WebSocket': 1 argument required");
            std::string raw = narrow(in.to_string(a[0]));
            std::string url =
                (raw.rfind("ws://", 0) == 0 ||
                 raw.rfind("wss://", 0) == 0)
                    ? raw
                    : self->resolve_url(widen(raw));
            if (url.rfind("https://", 0) == 0)
                url.replace(0, 5, "wss");
            else if (url.rfind("http://", 0) == 0)
                url.replace(0, 4, "ws");
            if (url.rfind("ws://", 0) != 0 &&
                url.rfind("wss://", 0) != 0)
                in.throw_error(
                    u"SyntaxError",
                    u"Failed to construct 'WebSocket': invalid URL scheme");

            JSObject* socket = in.new_object();
            socket->proto = websocket_proto;
            const int id = self->next_socket_id_++;
            socket->set(u"%socketId%", Value::make_int32(id), false);
            socket->set(u"url", in.str(url), false);
            socket->set(u"readyState", Value::make_int32(0), false);
            socket->set(u"bufferedAmount", Value::make_int32(0), false);
            socket->set(u"extensions", in.str(""), false);
            socket->set(u"protocol", in.str(""), false);
            socket->set(u"binaryType", in.str("blob"));
            socket->set(u"%listeners%", Value::make_heap_ptr(in.new_object()),
                        false);
            Value socket_value = Value::make_heap_ptr(socket);
            in.add_host_root(socket_value);
            self->sockets_.emplace_back(id, socket_value);

            if (self->socket_handler_) {
                self->socket_handler_(id, url, "", 0);
            } else {
                in.event_loop()->post_task(
                    [self, id] {
                        self->socket_close(
                            id, 1006,
                            "WebSocket transport is unavailable");
                    });
            }
            return Value::make_heap_ptr(socket);
        },
        1);
    websocket_ctor->set(
        u"prototype", Value::make_heap_ptr(websocket_proto), false);
    websocket_proto->set(
        u"constructor", Value::make_heap_ptr(websocket_ctor), false);
    for (const auto& [name, state] :
         std::initializer_list<std::pair<const char16_t*, int>>{
             {u"CONNECTING", 0}, {u"OPEN", 1},
             {u"CLOSING", 2}, {u"CLOSED", 3}}) {
        websocket_ctor->set(name, Value::make_int32(state), false);
        websocket_proto->set(name, Value::make_int32(state), false);
    }
    websocket_proto->set(
        u"addEventListener",
        Value::make_heap_ptr(interp.new_native(
            u"addEventListener",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (a.size() < 2 || !receiver.is_heap_ptr())
                    return Value::make_undefined();
                Value listeners_value =
                    in.get_prop_public(receiver, u"%listeners%");
                if (!listeners_value.is_heap_ptr() ||
                    listeners_value.as_heap_ptr()->kind !=
                        js::vm::HeapObject::kJSObject)
                    return Value::make_undefined();
                auto* listeners =
                    static_cast<JSObject*>(listeners_value.as_heap_ptr());
                std::u16string type = in.to_string(a[0]);
                Value array_value = listeners->get(type);
                JSArray* callbacks = nullptr;
                if (array_value.is_heap_ptr() &&
                    array_value.as_heap_ptr()->kind ==
                        js::vm::HeapObject::kJSArray) {
                    callbacks =
                        static_cast<JSArray*>(array_value.as_heap_ptr());
                } else {
                    callbacks = in.new_array();
                    listeners->set(type, Value::make_heap_ptr(callbacks));
                }
                for (Value callback : callbacks->elements)
                    if (callback == a[1]) return Value::make_undefined();
                callbacks->append(a[1]);
                return Value::make_undefined();
            },
            2)));
    websocket_proto->set(
        u"removeEventListener",
        Value::make_heap_ptr(interp.new_native(
            u"removeEventListener",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (a.size() < 2 || !receiver.is_heap_ptr())
                    return Value::make_undefined();
                Value listeners_value =
                    in.get_prop_public(receiver, u"%listeners%");
                if (!listeners_value.is_heap_ptr() ||
                    listeners_value.as_heap_ptr()->kind !=
                        js::vm::HeapObject::kJSObject)
                    return Value::make_undefined();
                auto* listeners =
                    static_cast<JSObject*>(listeners_value.as_heap_ptr());
                Value array_value =
                    listeners->get(in.to_string(a[0]));
                if (array_value.is_heap_ptr() &&
                    array_value.as_heap_ptr()->kind ==
                        js::vm::HeapObject::kJSArray) {
                    auto* callbacks =
                        static_cast<JSArray*>(array_value.as_heap_ptr());
                    for (size_t i = callbacks->elements.size(); i-- > 0;) {
                        if (callbacks->has_index(i) &&
                            callbacks->elements[i] == a[1])
                            callbacks->erase_range(i, 1);
                    }
                }
                return Value::make_undefined();
            },
            2)));
    websocket_proto->set(
        u"send",
        Value::make_heap_ptr(interp.new_native(
            u"send",
            [self](Interpreter& in, Value receiver,
                   std::vector<Value>& a) -> Value {
                if (in.to_int32(
                        in.get_prop_public(receiver, u"readyState")) != 1)
                    in.throw_error(
                        u"InvalidStateError",
                        u"WebSocket is not open");
                const int id = in.to_int32(
                    in.get_prop_public(receiver, u"%socketId%"));
                std::string data =
                    a.empty() ? std::string()
                              : narrow(in.to_string(a[0]));
                if (self->socket_handler_)
                    self->socket_handler_(id, "", data, 1);
                return Value::make_undefined();
            },
            1)));
    websocket_proto->set(
        u"close",
        Value::make_heap_ptr(interp.new_native(
            u"close",
            [self](Interpreter& in, Value receiver,
                   std::vector<Value>& a) -> Value {
                int state = in.to_int32(
                    in.get_prop_public(receiver, u"readyState"));
                if (state >= 2) return Value::make_undefined();
                const int id = in.to_int32(
                    in.get_prop_public(receiver, u"%socketId%"));
                const int code =
                    a.empty() || a[0].is_undefined()
                        ? 1000
                        : in.to_int32(a[0]);
                const std::string reason =
                    a.size() > 1 ? narrow(in.to_string(a[1]))
                                 : std::string();
                in.set_prop_public(
                    receiver, u"readyState", Value::make_int32(2));
                if (self->socket_handler_)
                    self->socket_handler_(
                        id, "", std::to_string(code) + "\n" + reason, 2);
                else
                    self->socket_close(id, code, reason);
                return Value::make_undefined();
            },
            2)));
    interp.global()->define(
        u"WebSocket", Value::make_heap_ptr(websocket_ctor));

    // ---- AbortController / AbortSignal ----------------------------------
    // Signals are EventTargets. The host fetch path is synchronous today, but
    // observing a pre-aborted signal still prevents the request from starting,
    // and timeout signals participate in the normal HTML event loop.
    JSObject* abort_signal_proto = interp.new_object();
    auto make_abort_reason =
        [](Interpreter& in, const char* name,
           const char* message) -> Value {
        JSObject* reason = in.new_object();
        reason->set(u"name", in.str(name));
        reason->set(u"message", in.str(message));
        reason->set(
            u"stack",
            in.str(std::string(name) + ": " + message));
        return Value::make_heap_ptr(reason);
    };
    auto make_abort_signal =
        [abort_signal_proto](Interpreter& in) -> Value {
        JSObject* signal = in.new_object();
        signal->proto = abort_signal_proto;
        signal->set(u"aborted", Value::make_bool(false), false);
        signal->set(u"reason", Value::make_undefined(), false);
        signal->set(u"onabort", Value::make_null());
        install_event_target(in, signal);
        return Value::make_heap_ptr(signal);
    };
    abort_signal_proto->set(
        u"throwIfAborted",
        Value::make_heap_ptr(interp.new_native(
            u"throwIfAborted",
            [](Interpreter& in, Value receiver,
               std::vector<Value>&) -> Value {
                Value aborted =
                    in.get_prop_public(receiver, u"aborted");
                if (aborted.is_bool() && aborted.as_bool())
                    throw js::runtime::ThrowSignal{
                        in.get_prop_public(receiver, u"reason")};
                return Value::make_undefined();
            })));
    JSFunction* abort_signal_ctor = interp.new_native(
        u"AbortSignal",
        [](Interpreter& in, Value,
           std::vector<Value>&) -> Value {
            in.throw_error(u"TypeError", u"Illegal constructor");
        });
    abort_signal_ctor->set(
        u"prototype",
        Value::make_heap_ptr(abort_signal_proto), false);
    abort_signal_proto->set(
        u"constructor",
        Value::make_heap_ptr(abort_signal_ctor), false);
    abort_signal_ctor->set(
        u"abort",
        Value::make_heap_ptr(interp.new_native(
            u"abort",
            [make_abort_signal, make_abort_reason](
                Interpreter& in, Value,
                std::vector<Value>& arguments) -> Value {
                Value signal = make_abort_signal(in);
                Value reason =
                    arguments.empty() || arguments[0].is_undefined()
                        ? make_abort_reason(
                              in, "AbortError",
                              "This operation was aborted")
                        : arguments[0];
                in.set_prop_public(
                    signal, u"aborted", Value::make_bool(true));
                in.set_prop_public(signal, u"reason", reason);
                return signal;
            },
            1)));
    abort_signal_ctor->set(
        u"timeout",
        Value::make_heap_ptr(interp.new_native(
            u"timeout",
            [make_abort_signal, make_abort_reason](
                Interpreter& in, Value,
                std::vector<Value>& arguments) -> Value {
                const double raw =
                    arguments.empty()
                        ? 0
                        : in.to_number(arguments[0]);
                if (!std::isfinite(raw) || raw < 0)
                    in.throw_error(
                        u"RangeError",
                        u"AbortSignal.timeout delay must be non-negative");
                Value signal = make_abort_signal(in);
                in.add_host_root(signal);
                Interpreter* interpreter = &in;
                in.event_loop()->set_timeout(
                    [interpreter, signal, make_abort_reason] {
                        interpreter->set_prop_public(
                            signal, u"aborted",
                            Value::make_bool(true));
                        interpreter->set_prop_public(
                            signal, u"reason",
                            make_abort_reason(
                                *interpreter, "TimeoutError",
                                "The operation timed out"));
                        dispatch_event_target(
                            *interpreter, signal, u"abort",
                            Value::make_undefined());
                        interpreter->remove_host_root(signal);
                    },
                    static_cast<uint64_t>(raw));
                return signal;
            },
            1)));
    interp.global()->define(
        u"AbortSignal",
        Value::make_heap_ptr(abort_signal_ctor));

    JSObject* abort_controller_proto = interp.new_object();
    JSFunction* abort_controller_ctor = interp.new_native(
        u"AbortController",
        [abort_controller_proto, make_abort_signal](
            Interpreter& in, Value,
            std::vector<Value>&) -> Value {
            JSObject* controller = in.new_object();
            controller->proto = abort_controller_proto;
            controller->set(u"signal", make_abort_signal(in), false);
            return Value::make_heap_ptr(controller);
        });
    abort_controller_ctor->set(
        u"prototype",
        Value::make_heap_ptr(abort_controller_proto), false);
    abort_controller_proto->set(
        u"constructor",
        Value::make_heap_ptr(abort_controller_ctor), false);
    abort_controller_proto->set(
        u"abort",
        Value::make_heap_ptr(interp.new_native(
            u"abort",
            [make_abort_reason](
                Interpreter& in, Value receiver,
                std::vector<Value>& arguments) -> Value {
                Value signal =
                    in.get_prop_public(receiver, u"signal");
                if (!signal.is_heap_ptr())
                    in.throw_error(
                        u"TypeError",
                        u"AbortController.abort called on incompatible receiver");
                Value aborted =
                    in.get_prop_public(signal, u"aborted");
                if (aborted.is_bool() && aborted.as_bool())
                    return Value::make_undefined();
                Value reason =
                    arguments.empty() || arguments[0].is_undefined()
                        ? make_abort_reason(
                              in, "AbortError",
                              "This operation was aborted")
                        : arguments[0];
                in.set_prop_public(
                    signal, u"aborted", Value::make_bool(true));
                in.set_prop_public(signal, u"reason", reason);
                dispatch_event_target(
                    in, signal, u"abort",
                    Value::make_undefined());
                return Value::make_undefined();
            },
            1)));
    interp.global()->define(
        u"AbortController",
        Value::make_heap_ptr(abort_controller_ctor));

    // ---- Web Storage (localStorage / sessionStorage), per-origin ----
    interp.global()->define(u"localStorage",
        make_storage_object(
            interp,
            [self]() -> storage::Storage& {
                return self->storage_.local_storage(self->origin_);
            }));
    interp.global()->define(u"sessionStorage",
        make_storage_object(
            interp,
            [self]() -> storage::Storage& {
                return self->storage_.session_storage(
                    self->origin_, "");
            }));

    // ---- fetch() -> Promise<Response> (satisfied via the host request handler) ----
    Value json_parse = Value::make_undefined();
    if (Value* j = interp.global()->find(u"JSON"))
        if (j->is_heap_ptr() && j->as_heap_ptr()->kind == js::vm::HeapObject::kJSObject)
            json_parse = static_cast<JSObject*>(j->as_heap_ptr())->get(u"parse");

    // ---- Fetch data classes + Cache API ----
    JSObject* headers_proto = interp.new_object();
    JSFunction* headers_ctor = interp.new_native(
        u"Headers",
        [headers_proto](Interpreter& in, Value,
                        std::vector<Value>& a) -> Value {
            JSObject* headers = in.new_object();
            headers->proto = headers_proto;
            JSObject* values = in.new_object();
            headers->set(u"%values%", Value::make_heap_ptr(values), false);
            auto mutate = [](bool append) {
                return [append](Interpreter& in2, Value receiver,
                                std::vector<Value>& ca) -> Value {
                    Value values_value =
                        in2.get_prop_public(receiver, u"%values%");
                    if (!values_value.is_heap_ptr() ||
                        values_value.as_heap_ptr()->kind !=
                            js::vm::HeapObject::kJSObject)
                        in2.throw_error(
                            u"TypeError",
                            u"Headers method called on incompatible receiver");
                    auto* map =
                        static_cast<JSObject*>(
                            values_value.as_heap_ptr());
                    std::u16string name =
                        ca.empty() ? u"" : in2.to_string(ca[0]);
                    std::transform(
                        name.begin(), name.end(), name.begin(),
                        [](char16_t c) {
                            return c >= u'A' && c <= u'Z'
                                ? static_cast<char16_t>(
                                      c - u'A' + u'a')
                                : c;
                        });
                    std::u16string value =
                        ca.size() > 1 ? in2.to_string(ca[1]) : u"";
                    if (append && map->has(name)) {
                        std::u16string old =
                            in2.to_string(map->get(name));
                        if (!old.empty()) old += u", ";
                        value = old + value;
                    }
                    map->set(name, in2.str(narrow(value)));
                    return Value::make_undefined();
                };
            };
            headers->set(
                u"append",
                Value::make_heap_ptr(
                    in.new_native(u"append", mutate(true), 2)));
            headers->set(
                u"set",
                Value::make_heap_ptr(
                    in.new_native(u"set", mutate(false), 2)));
            headers->set(
                u"get",
                Value::make_heap_ptr(in.new_native(
                    u"get",
                    [](Interpreter& in2, Value receiver,
                       std::vector<Value>& ca) -> Value {
                        Value values_value =
                            in2.get_prop_public(receiver, u"%values%");
                        if (!values_value.is_heap_ptr() ||
                            values_value.as_heap_ptr()->kind !=
                                js::vm::HeapObject::kJSObject)
                            in2.throw_error(
                                u"TypeError",
                                u"Headers.get called on incompatible receiver");
                        std::u16string name =
                            ca.empty() ? u"" : in2.to_string(ca[0]);
                        std::transform(
                            name.begin(), name.end(), name.begin(),
                            [](char16_t c) {
                                return c >= u'A' && c <= u'Z'
                                    ? static_cast<char16_t>(
                                          c - u'A' + u'a')
                                    : c;
                            });
                        auto* map = static_cast<JSObject*>(
                            values_value.as_heap_ptr());
                        return map->has(name)
                            ? map->get(name)
                            : Value::make_null();
                    },
                    1)));
            if (!a.empty() && a[0].is_heap_ptr()) {
                Value source_values =
                    in.get_prop_public(a[0], u"%values%");
                if (source_values.is_heap_ptr() &&
                    source_values.as_heap_ptr()->kind ==
                        js::vm::HeapObject::kJSObject)
                    headers->set(u"%values%", source_values, false);
            }
            return Value::make_heap_ptr(headers);
        });
    headers_ctor->set(
        u"prototype", Value::make_heap_ptr(headers_proto), false);
    headers_proto->set(
        u"constructor", Value::make_heap_ptr(headers_ctor), false);
    interp.global()->define(
        u"Headers", Value::make_heap_ptr(headers_ctor));

    JSObject* request_proto = interp.new_object();
    JSFunction* request_ctor = interp.new_native(
        u"Request",
        [self, request_proto, headers_ctor](Interpreter& in, Value,
                              std::vector<Value>& a) -> Value {
            JSObject* request = in.new_object();
            request->proto = request_proto;
            std::string raw;
            if (!a.empty() && a[0].is_heap_ptr()) {
                Value existing = in.get_prop_public(a[0], u"url");
                if (!existing.is_undefined())
                    raw = narrow(in.to_string(existing));
            }
            if (raw.empty() && !a.empty())
                raw = narrow(in.to_string(a[0]));
            request->set(
                u"url",
                in.str(self->resolve_url(widen(raw))));
            request->set(u"method", in.str("GET"));
            std::vector<Value> no_arguments;
            request->set(
                u"headers",
                in.construct(
                    Value::make_heap_ptr(headers_ctor),
                    no_arguments));
            if (a.size() > 1 && a[1].is_heap_ptr()) {
                Value method = in.get_prop_public(a[1], u"method");
                if (!method.is_undefined())
                    request->set(u"method", method);
                Value headers = in.get_prop_public(a[1], u"headers");
                if (!headers.is_undefined())
                    request->set(u"headers", headers);
                Value body = in.get_prop_public(a[1], u"body");
                if (!body.is_undefined())
                    request->set(u"body", body);
                Value signal = in.get_prop_public(a[1], u"signal");
                if (!signal.is_undefined())
                    request->set(u"signal", signal);
            }
            return Value::make_heap_ptr(request);
        },
        1);
    request_ctor->set(
        u"prototype", Value::make_heap_ptr(request_proto), false);
    request_proto->set(
        u"constructor", Value::make_heap_ptr(request_ctor), false);
    interp.global()->define(
        u"Request", Value::make_heap_ptr(request_ctor));

    JSObject* response_proto = interp.new_object();
    auto bytes_from_body = [](Interpreter& in,
                              Value body) -> std::vector<uint8_t> {
        if (body.is_null() || body.is_undefined()) return {};
        if (body.is_heap_ptr() &&
            body.as_heap_ptr()->kind ==
                js::vm::HeapObject::kArrayBuffer)
            return static_cast<JSArrayBuffer*>(
                       body.as_heap_ptr())
                ->data;
        if (body.is_heap_ptr() &&
            body.as_heap_ptr()->kind ==
                js::vm::HeapObject::kTypedArray) {
            auto* typed =
                static_cast<JSTypedArray*>(body.as_heap_ptr());
            if (typed->buffer)
                return std::vector<uint8_t>(
                    typed->buffer->data.begin() +
                        typed->byte_offset,
                    typed->buffer->data.begin() +
                        typed->byte_offset +
                        typed->byte_length());
        }
        std::string text = narrow(in.to_string(body));
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    auto make_response =
        [response_proto, json_parse, headers_ctor](
            Interpreter& in, std::vector<uint8_t> bytes,
            int32_t status, const std::string& url) -> Value {
        JSObject* response = in.new_object();
        response->proto = response_proto;
        auto* body_buffer = in.new_array_buffer(bytes);
        response->set(
            u"%bodyBuffer%", Value::make_heap_ptr(body_buffer), false);
        response->set(u"status", Value::make_int32(status));
        response->set(
            u"ok", Value::make_bool(status >= 200 && status < 300));
        response->set(u"url", in.str(url));
        std::vector<Value> no_arguments;
        response->set(
            u"headers",
            in.construct(
                Value::make_heap_ptr(headers_ctor),
                no_arguments));
        response->set(
            u"text",
            Value::make_heap_ptr(in.new_native(
                u"text",
                [body_buffer](Interpreter& in2, Value,
                              std::vector<Value>&) -> Value {
                    JSPromise* promise = in2.new_promise();
                    std::string body(
                        body_buffer->data.begin(),
                        body_buffer->data.end());
                    in2.resolve_promise(promise, in2.str(body));
                    return Value::make_heap_ptr(promise);
                })));
        response->set(
            u"json",
            Value::make_heap_ptr(in.new_native(
                u"json",
                [body_buffer, json_parse](
                    Interpreter& in2, Value,
                    std::vector<Value>&) -> Value {
                    JSPromise* promise = in2.new_promise();
                    std::string body(
                        body_buffer->data.begin(),
                        body_buffer->data.end());
                    std::vector<Value> args{in2.str(body)};
                    Value parsed =
                        json_parse.is_undefined()
                            ? Value::make_undefined()
                            : in2.call(
                                  json_parse,
                                  Value::make_undefined(), args);
                    in2.resolve_promise(promise, parsed);
                    return Value::make_heap_ptr(promise);
                })));
        response->set(
            u"arrayBuffer",
            Value::make_heap_ptr(in.new_native(
                u"arrayBuffer",
                [body_buffer](Interpreter& in2, Value,
                              std::vector<Value>&) -> Value {
                    JSPromise* promise = in2.new_promise();
                    in2.resolve_promise(
                        promise,
                        Value::make_heap_ptr(
                            in2.new_array_buffer(
                                body_buffer->data)));
                    return Value::make_heap_ptr(promise);
                })));
        return Value::make_heap_ptr(response);
    };
    JSFunction* response_ctor = interp.new_native(
        u"Response",
        [bytes_from_body, make_response, headers_ctor](
            Interpreter& in, Value,
            std::vector<Value>& a) -> Value {
            std::vector<uint8_t> bytes =
                bytes_from_body(
                    in, a.empty() ? Value::make_null() : a[0]);
            int32_t status = 200;
            if (a.size() > 1 && a[1].is_heap_ptr()) {
                Value status_value =
                    in.get_prop_public(a[1], u"status");
                if (!status_value.is_undefined())
                    status = in.to_int32(status_value);
            }
            Value response =
                make_response(in, std::move(bytes), status, "");
            if (a.size() > 1 && a[1].is_heap_ptr()) {
                Value headers =
                    in.get_prop_public(a[1], u"headers");
                if (!headers.is_undefined()) {
                    std::vector<Value> header_arguments{headers};
                    in.set_prop_public(
                        response, u"headers",
                        in.construct(
                            Value::make_heap_ptr(headers_ctor),
                            header_arguments));
                }
            }
            return response;
        },
        1);
    response_ctor->set(
        u"prototype", Value::make_heap_ptr(response_proto), false);
    response_proto->set(
        u"constructor", Value::make_heap_ptr(response_ctor), false);
    interp.global()->define(
        u"Response", Value::make_heap_ptr(response_ctor));

    JSObject* cache_proto = interp.new_object();
    JSFunction* cache_ctor = interp.new_native(
        u"Cache",
        [](Interpreter& in, Value,
           std::vector<Value>&) -> Value {
            in.throw_error(u"TypeError", u"Illegal constructor");
        });
    cache_ctor->set(
        u"prototype", Value::make_heap_ptr(cache_proto), false);
    cache_proto->set(
        u"constructor", Value::make_heap_ptr(cache_ctor), false);
    interp.global()->define(
        u"Cache", Value::make_heap_ptr(cache_ctor));

    auto request_url = [self](Interpreter& in, Value request) {
        if (request.is_heap_ptr()) {
            Value url = in.get_prop_public(request, u"url");
            if (!url.is_undefined())
                return narrow(in.to_string(url));
        }
        return self->resolve_url(
            widen(narrow(in.to_string(request))));
    };
    auto make_cache =
        [self, cache_proto, request_url,
         make_response](Interpreter& in,
                        const std::string& name) -> Value {
        JSObject* cache = in.new_object();
        cache->proto = cache_proto;
        cache->set(u"%name%", in.str(name), false);
        cache->set(
            u"match",
            Value::make_heap_ptr(in.new_native(
                u"match",
                [self, name, request_url, make_response](
                    Interpreter& in2, Value,
                    std::vector<Value>& a) -> Value {
                    JSPromise* promise = in2.new_promise();
                    std::string url = request_url(
                        in2, a.empty()
                                 ? Value::make_undefined()
                                 : a[0]);
                    auto found =
                        self->storage_.cache_storage(self->origin_)
                            .match(name, url);
                    if (!found) {
                        in2.resolve_promise(
                            promise, Value::make_undefined());
                    } else {
                        in2.resolve_promise(
                            promise,
                            make_response(
                                in2, found->body, found->status,
                                url));
                    }
                    return Value::make_heap_ptr(promise);
                },
                1)));
        cache->set(
            u"put",
            Value::make_heap_ptr(in.new_native(
                u"put",
                [self, name, request_url](
                    Interpreter& in2, Value,
                    std::vector<Value>& a) -> Value {
                    JSPromise* promise = in2.new_promise();
                    std::string url = request_url(
                        in2, a.empty()
                                 ? Value::make_undefined()
                                 : a[0]);
                    storage::CacheStorage::CachedResponse stored;
                    if (a.size() > 1 && a[1].is_heap_ptr()) {
                        Value status =
                            in2.get_prop_public(a[1], u"status");
                        if (!status.is_undefined())
                            stored.status =
                                in2.to_int32(status);
                        Value body = in2.get_prop_public(
                            a[1], u"%bodyBuffer%");
                        if (body.is_heap_ptr() &&
                            body.as_heap_ptr()->kind ==
                                js::vm::HeapObject::kArrayBuffer)
                            stored.body =
                                static_cast<JSArrayBuffer*>(
                                    body.as_heap_ptr())
                                    ->data;
                    }
                    self->storage_.cache_storage(self->origin_)
                        .put(name, url, std::move(stored));
                    in2.resolve_promise(
                        promise, Value::make_undefined());
                    return Value::make_heap_ptr(promise);
                },
                2)));
        cache->set(
            u"delete",
            Value::make_heap_ptr(in.new_native(
                u"delete",
                [self, name, request_url](
                    Interpreter& in2, Value,
                    std::vector<Value>& a) -> Value {
                    JSPromise* promise = in2.new_promise();
                    bool removed =
                        self->storage_.cache_storage(self->origin_)
                            .delete_entry(
                                name,
                                request_url(
                                    in2, a.empty()
                                             ? Value::make_undefined()
                                             : a[0]));
                    in2.resolve_promise(
                        promise, Value::make_bool(removed));
                    return Value::make_heap_ptr(promise);
                },
                1)));
        return Value::make_heap_ptr(cache);
    };
    JSObject* caches = interp.new_object();
    caches->set(
        u"open",
        Value::make_heap_ptr(interp.new_native(
            u"open",
            [self, make_cache](Interpreter& in, Value,
                               std::vector<Value>& a) -> Value {
                std::string name =
                    a.empty() ? "" : narrow(in.to_string(a[0]));
                self->storage_.cache_storage(self->origin_)
                    .open(name);
                JSPromise* promise = in.new_promise();
                in.resolve_promise(
                    promise, make_cache(in, name));
                return Value::make_heap_ptr(promise);
            },
            1)));
    interp.global()->define(
        u"caches", Value::make_heap_ptr(caches));

    auto* fetch_fn = interp.new_native(u"fetch",
        [self, make_response, request_url](
            Interpreter& in, Value,
            std::vector<Value>& a) -> Value {
            Value input =
                a.empty() ? Value::make_undefined() : a[0];
            std::string url = request_url(in, input);
            JSPromise* p = in.new_promise();
            Value signal = Value::make_undefined();
            if (a.size() > 1 && a[1].is_heap_ptr())
                signal =
                    in.get_prop_public(a[1], u"signal");
            if (signal.is_undefined() && input.is_heap_ptr())
                signal =
                    in.get_prop_public(input, u"signal");
            if (signal.is_heap_ptr()) {
                Value aborted =
                    in.get_prop_public(signal, u"aborted");
                if (aborted.is_bool() && aborted.as_bool()) {
                    in.reject_promise(
                        p, in.get_prop_public(signal, u"reason"));
                    return Value::make_heap_ptr(p);
                }
            }
            network::FetchResponse resp;
            network::FetchRequest request;
            request.url = url;
            request.referrer = self->current_url_;
            bool handled =
                self->perform_request(request, resp);
            if (!handled) {
                in.reject_promise(p, in.str(std::string("TypeError: Failed to fetch: ") + url));
                return Value::make_heap_ptr(p);
            }
            int32_t status = resp.status ? resp.status : 200;
            in.resolve_promise(
                p, make_response(
                       in, std::move(resp.body), status, url));
            return Value::make_heap_ptr(p);
        });
    interp.global()->define(u"fetch", Value::make_heap_ptr(fetch_fn));

    // ---- XMLHttpRequest -------------------------------------------------
    // Network I/O is supplied by the host, while state changes and events are
    // delivered from an HTML event-loop task for the normal asynchronous mode.
    auto dispatch_xhr_event =
        [](Interpreter& in, Value target,
           const std::u16string& type, size_t loaded = 0,
           size_t total = 0) {
            JSObject* event = in.new_object();
            event->set(u"type", in.str(type));
            event->set(u"target", target);
            event->set(u"currentTarget", target);
            event->set(u"lengthComputable",
                       Value::make_bool(total != 0));
            event->set(
                u"loaded",
                loaded <= static_cast<size_t>(
                              std::numeric_limits<int32_t>::max())
                    ? Value::make_int32(
                          static_cast<int32_t>(loaded))
                    : Value::make_double(
                          static_cast<double>(loaded)));
            event->set(
                u"total",
                total <= static_cast<size_t>(
                             std::numeric_limits<int32_t>::max())
                    ? Value::make_int32(
                          static_cast<int32_t>(total))
                    : Value::make_double(
                          static_cast<double>(total)));
            event->set(
                u"preventDefault",
                Value::make_heap_ptr(in.new_native(
                    u"preventDefault",
                    [](Interpreter&, Value,
                       std::vector<Value>&) {
                        return Value::make_undefined();
                    })));
            Value event_value = Value::make_heap_ptr(event);
            auto invoke = [&](Value callback) {
                if (!in.is_callable(callback)) return;
                std::vector<Value> args{event_value};
                try {
                    in.call(callback, target, args);
                } catch (const js::runtime::ThrowSignal&) {
                    // Event listener exceptions are reported independently by
                    // browsers and do not stop the XHR state machine.
                }
            };

            invoke(in.get_prop_public(target, u"on" + type));
            Value listeners_value =
                in.get_prop_public(target, u"%listeners%");
            if (!listeners_value.is_heap_ptr() ||
                listeners_value.as_heap_ptr()->kind !=
                    js::vm::HeapObject::kJSObject)
                return;
            Value callbacks_value =
                static_cast<JSObject*>(
                    listeners_value.as_heap_ptr())
                    ->get(type);
            if (!callbacks_value.is_heap_ptr() ||
                callbacks_value.as_heap_ptr()->kind !=
                    js::vm::HeapObject::kJSArray)
                return;
            auto callbacks =
                static_cast<JSArray*>(
                    callbacks_value.as_heap_ptr())
                    ->elements;
            for (Value callback : callbacks) invoke(callback);
        };

    JSObject* xhr_event_target_proto = interp.new_object();
    xhr_event_target_proto->set(
        u"addEventListener",
        Value::make_heap_ptr(interp.new_native(
            u"addEventListener",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (a.size() < 2 || !in.is_callable(a[1]))
                    return Value::make_undefined();
                Value listeners_value =
                    in.get_prop_public(receiver, u"%listeners%");
                if (!listeners_value.is_heap_ptr() ||
                    listeners_value.as_heap_ptr()->kind !=
                        js::vm::HeapObject::kJSObject)
                    return Value::make_undefined();
                auto* listeners = static_cast<JSObject*>(
                    listeners_value.as_heap_ptr());
                std::u16string type = in.to_string(a[0]);
                Value callbacks_value = listeners->get(type);
                JSArray* callbacks = nullptr;
                if (callbacks_value.is_heap_ptr() &&
                    callbacks_value.as_heap_ptr()->kind ==
                        js::vm::HeapObject::kJSArray) {
                    callbacks = static_cast<JSArray*>(
                        callbacks_value.as_heap_ptr());
                } else {
                    callbacks = in.new_array();
                    listeners->set(
                        type, Value::make_heap_ptr(callbacks));
                }
                for (Value callback : callbacks->elements)
                    if (callback == a[1])
                        return Value::make_undefined();
                callbacks->append(a[1]);
                return Value::make_undefined();
            },
            2)));
    xhr_event_target_proto->set(
        u"removeEventListener",
        Value::make_heap_ptr(interp.new_native(
            u"removeEventListener",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (a.size() < 2)
                    return Value::make_undefined();
                Value listeners_value =
                    in.get_prop_public(receiver, u"%listeners%");
                if (!listeners_value.is_heap_ptr() ||
                    listeners_value.as_heap_ptr()->kind !=
                        js::vm::HeapObject::kJSObject)
                    return Value::make_undefined();
                Value callbacks_value =
                    static_cast<JSObject*>(
                        listeners_value.as_heap_ptr())
                        ->get(in.to_string(a[0]));
                if (!callbacks_value.is_heap_ptr() ||
                    callbacks_value.as_heap_ptr()->kind !=
                        js::vm::HeapObject::kJSArray)
                    return Value::make_undefined();
                auto* callbacks = static_cast<JSArray*>(
                    callbacks_value.as_heap_ptr());
                for (size_t i = callbacks->elements.size();
                     i-- > 0;) {
                    if (callbacks->has_index(i) &&
                        callbacks->elements[i] == a[1])
                        callbacks->erase_range(i, 1);
                }
                return Value::make_undefined();
            },
            2)));
    xhr_event_target_proto->set(
        u"dispatchEvent",
        Value::make_heap_ptr(interp.new_native(
            u"dispatchEvent",
            [dispatch_xhr_event](
                Interpreter& in, Value receiver,
                std::vector<Value>& a) -> Value {
                if (a.empty())
                    in.throw_error(
                        u"TypeError",
                        u"Failed to execute 'dispatchEvent': "
                        u"1 argument required");
                std::u16string type =
                    in.to_string(
                        in.get_prop_public(a[0], u"type"));
                dispatch_xhr_event(in, receiver, type);
                return Value::make_bool(true);
            },
            1)));

    JSObject* xhr_proto = interp.new_object();
    xhr_proto->proto = xhr_event_target_proto;
    JSFunction* xhr_ctor = interp.new_native(
        u"XMLHttpRequest",
        [xhr_proto, xhr_event_target_proto](
            Interpreter& in, Value,
            std::vector<Value>&) -> Value {
            JSObject* xhr = in.new_object();
            xhr->proto = xhr_proto;
            xhr->set(
                u"%listeners%",
                Value::make_heap_ptr(in.new_object()), false);
            xhr->set(
                u"%requestHeaders%",
                Value::make_heap_ptr(in.new_object()), false);
            xhr->set(
                u"%responseHeaders%",
                Value::make_heap_ptr(in.new_object()), false);
            xhr->set(u"%method%", in.str("GET"), false);
            xhr->set(u"%url%", in.str(""), false);
            xhr->set(u"%async%", Value::make_bool(true), false);
            xhr->set(u"%aborted%", Value::make_bool(false), false);
            xhr->set(u"readyState", Value::make_int32(0));
            xhr->set(u"response", in.str(""));
            xhr->set(u"responseText", in.str(""));
            xhr->set(u"responseType", in.str(""));
            xhr->set(u"responseURL", in.str(""));
            xhr->set(u"responseXML", Value::make_null());
            xhr->set(u"status", Value::make_int32(0));
            xhr->set(u"statusText", in.str(""));
            xhr->set(u"timeout", Value::make_int32(0));
            xhr->set(u"withCredentials", Value::make_bool(false));

            JSObject* upload = in.new_object();
            upload->proto = xhr_event_target_proto;
            upload->set(
                u"%listeners%",
                Value::make_heap_ptr(in.new_object()), false);
            xhr->set(u"upload", Value::make_heap_ptr(upload));
            return Value::make_heap_ptr(xhr);
        });
    xhr_ctor->set(
        u"prototype", Value::make_heap_ptr(xhr_proto), false);
    xhr_proto->set(
        u"constructor", Value::make_heap_ptr(xhr_ctor), false);
    for (const auto& [name, state] :
         std::initializer_list<
             std::pair<const char16_t*, int>>{
             {u"UNSENT", 0}, {u"OPENED", 1},
             {u"HEADERS_RECEIVED", 2}, {u"LOADING", 3},
             {u"DONE", 4}}) {
        xhr_ctor->set(
            name, Value::make_int32(state), false);
        xhr_proto->set(
            name, Value::make_int32(state), false);
    }

    xhr_proto->set(
        u"open",
        Value::make_heap_ptr(interp.new_native(
            u"open",
            [self, dispatch_xhr_event](
                Interpreter& in, Value receiver,
                std::vector<Value>& a) -> Value {
                if (a.size() < 2)
                    in.throw_error(
                        u"TypeError",
                        u"Failed to execute 'open' on "
                        u"'XMLHttpRequest': 2 arguments required");
                std::string method =
                    narrow(in.to_string(a[0]));
                std::transform(
                    method.begin(), method.end(),
                    method.begin(),
                    [](unsigned char c) {
                        return static_cast<char>(
                            std::toupper(c));
                    });
                const std::string raw_url =
                    narrow(in.to_string(a[1]));
                const bool async =
                    a.size() < 3 || a[2].is_undefined() ||
                    in.to_bool(a[2]);
                in.set_prop_public(
                    receiver, u"%method%",
                    in.str(method));
                in.set_prop_public(
                    receiver, u"%url%",
                    in.str(self->resolve_url(
                        widen(raw_url))));
                in.set_prop_public(
                    receiver, u"%async%",
                    Value::make_bool(async));
                in.set_prop_public(
                    receiver, u"%aborted%",
                    Value::make_bool(false));
                in.set_prop_public(
                    receiver, u"%requestHeaders%",
                    Value::make_heap_ptr(
                        in.new_object()));
                in.set_prop_public(
                    receiver, u"status",
                    Value::make_int32(0));
                in.set_prop_public(
                    receiver, u"statusText",
                    in.str(""));
                in.set_prop_public(
                    receiver, u"responseText",
                    in.str(""));
                in.set_prop_public(
                    receiver, u"response",
                    in.str(""));
                in.set_prop_public(
                    receiver, u"readyState",
                    Value::make_int32(1));
                dispatch_xhr_event(
                    in, receiver, u"readystatechange");
                return Value::make_undefined();
            },
            2)));
    xhr_proto->set(
        u"setRequestHeader",
        Value::make_heap_ptr(interp.new_native(
            u"setRequestHeader",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (a.size() < 2 ||
                    in.to_int32(in.get_prop_public(
                        receiver, u"readyState")) != 1)
                    in.throw_error(
                        u"InvalidStateError",
                        u"XMLHttpRequest is not opened");
                Value headers_value =
                    in.get_prop_public(
                        receiver, u"%requestHeaders%");
                auto* headers = static_cast<JSObject*>(
                    headers_value.as_heap_ptr());
                std::u16string name =
                    in.to_string(a[0]);
                std::transform(
                    name.begin(), name.end(), name.begin(),
                    [](char16_t c) {
                        return c >= u'A' && c <= u'Z'
                            ? static_cast<char16_t>(
                                  c - u'A' + u'a')
                            : c;
                    });
                std::u16string value =
                    in.to_string(a[1]);
                if (headers->has_own(name)) {
                    std::u16string old =
                        in.to_string(headers->get(name));
                    if (!old.empty()) old += u", ";
                    value = old + value;
                }
                headers->set(name, in.str(value));
                return Value::make_undefined();
            },
            2)));
    xhr_proto->set(
        u"getResponseHeader",
        Value::make_heap_ptr(interp.new_native(
            u"getResponseHeader",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (in.to_int32(in.get_prop_public(
                        receiver, u"readyState")) < 2)
                    return Value::make_null();
                std::u16string name =
                    a.empty() ? u"" : in.to_string(a[0]);
                std::transform(
                    name.begin(), name.end(), name.begin(),
                    [](char16_t c) {
                        return c >= u'A' && c <= u'Z'
                            ? static_cast<char16_t>(
                                  c - u'A' + u'a')
                            : c;
                    });
                Value headers_value =
                    in.get_prop_public(
                        receiver, u"%responseHeaders%");
                auto* headers = static_cast<JSObject*>(
                    headers_value.as_heap_ptr());
                return headers->has_own(name)
                    ? headers->get(name)
                    : Value::make_null();
            },
            1)));
    xhr_proto->set(
        u"getAllResponseHeaders",
        Value::make_heap_ptr(interp.new_native(
            u"getAllResponseHeaders",
            [](Interpreter& in, Value receiver,
               std::vector<Value>&) -> Value {
                if (in.to_int32(in.get_prop_public(
                        receiver, u"readyState")) < 2)
                    return in.str("");
                return in.get_prop_public(
                    receiver, u"%responseHeaderBlock%");
            })));
    xhr_proto->set(
        u"overrideMimeType",
        Value::make_heap_ptr(interp.new_native(
            u"overrideMimeType",
            [](Interpreter& in, Value receiver,
               std::vector<Value>& a) -> Value {
                if (!a.empty())
                    in.set_prop_public(
                        receiver, u"%overrideMimeType%",
                        a[0]);
                return Value::make_undefined();
            },
            1)));
    xhr_proto->set(
        u"abort",
        Value::make_heap_ptr(interp.new_native(
            u"abort",
            [dispatch_xhr_event](
                Interpreter& in, Value receiver,
                std::vector<Value>&) -> Value {
                in.set_prop_public(
                    receiver, u"%aborted%",
                    Value::make_bool(true));
                in.set_prop_public(
                    receiver, u"status",
                    Value::make_int32(0));
                in.set_prop_public(
                    receiver, u"readyState",
                    Value::make_int32(0));
                dispatch_xhr_event(
                    in, receiver, u"abort");
                dispatch_xhr_event(
                    in, receiver, u"loadend");
                return Value::make_undefined();
            })));
    xhr_proto->set(
        u"send",
        Value::make_heap_ptr(interp.new_native(
            u"send",
            [self, bytes_from_body, json_parse,
             dispatch_xhr_event](
                Interpreter& in, Value receiver,
                std::vector<Value>& a) -> Value {
                if (in.to_int32(in.get_prop_public(
                        receiver, u"readyState")) != 1)
                    in.throw_error(
                        u"InvalidStateError",
                        u"XMLHttpRequest is not opened");

                network::FetchRequest request;
                request.method = narrow(in.to_string(
                    in.get_prop_public(
                        receiver, u"%method%")));
                request.url = narrow(in.to_string(
                    in.get_prop_public(
                        receiver, u"%url%")));
                request.referrer = self->current_url_;
                request.credentials =
                    in.to_bool(in.get_prop_public(
                        receiver, u"withCredentials"))
                        ? network::CredentialsMode::Include
                        : network::CredentialsMode::SameOrigin;
                Value headers_value =
                    in.get_prop_public(
                        receiver, u"%requestHeaders%");
                if (headers_value.is_heap_ptr() &&
                    headers_value.as_heap_ptr()->kind ==
                        js::vm::HeapObject::kJSObject) {
                    for (const auto& property :
                         static_cast<JSObject*>(
                             headers_value.as_heap_ptr())
                             ->props) {
                        if (!property.enumerable) continue;
                        request.headers.set(
                            narrow(property.key),
                            narrow(in.to_string(
                                property.value)));
                    }
                }
                if (!a.empty() && !a[0].is_null() &&
                    !a[0].is_undefined())
                    request.body =
                        bytes_from_body(in, a[0]);

                in.set_prop_public(
                    receiver, u"%aborted%",
                    Value::make_bool(false));
                dispatch_xhr_event(
                    in, receiver, u"loadstart");

                auto complete =
                    [self, receiver,
                     request = std::move(request),
                     json_parse, dispatch_xhr_event]() mutable {
                        auto& in2 =
                            self->engine_.interpreter();
                        if (in2.to_bool(
                                in2.get_prop_public(
                                    receiver,
                                    u"%aborted%")))
                            return;

                        network::FetchResponse response;
                        const bool handled =
                            self->perform_request(
                                request, response);
                        if (!handled) {
                            in2.set_prop_public(
                                receiver, u"status",
                                Value::make_int32(0));
                            in2.set_prop_public(
                                receiver, u"readyState",
                                Value::make_int32(4));
                            dispatch_xhr_event(
                                in2, receiver,
                                u"readystatechange");
                            dispatch_xhr_event(
                                in2, receiver, u"error");
                            dispatch_xhr_event(
                                in2, receiver, u"loadend");
                            return;
                        }

                        const int status =
                            response.status == 0
                                ? 200
                                : response.status;
                        in2.set_prop_public(
                            receiver, u"status",
                            Value::make_int32(status));
                        in2.set_prop_public(
                            receiver, u"statusText",
                            in2.str(response.status_text));
                        in2.set_prop_public(
                            receiver, u"responseURL",
                            in2.str(
                                response.url.empty()
                                    ? request.url
                                    : response.url));

                        JSObject* response_headers =
                            in2.new_object();
                        std::string header_block;
                        for (const auto& [name, value] :
                             response.headers.entries) {
                            response_headers->set(
                                widen(name),
                                in2.str(widen(value)));
                            header_block += name + ": " +
                                            value + "\r\n";
                        }
                        in2.set_prop_public(
                            receiver,
                            u"%responseHeaders%",
                            Value::make_heap_ptr(
                                response_headers));
                        in2.set_prop_public(
                            receiver,
                            u"%responseHeaderBlock%",
                            in2.str(widen(header_block)));
                        in2.set_prop_public(
                            receiver, u"readyState",
                            Value::make_int32(2));
                        dispatch_xhr_event(
                            in2, receiver,
                            u"readystatechange");
                        in2.set_prop_public(
                            receiver, u"readyState",
                            Value::make_int32(3));
                        dispatch_xhr_event(
                            in2, receiver,
                            u"readystatechange");

                        const std::string text(
                            response.body.begin(),
                            response.body.end());
                        const std::u16string response_type =
                            in2.to_string(
                                in2.get_prop_public(
                                    receiver,
                                    u"responseType"));
                        if (response_type == u"arraybuffer" ||
                            response_type == u"blob") {
                            Value buffer =
                                Value::make_heap_ptr(
                                    in2.new_array_buffer(
                                        response.body));
                            in2.set_prop_public(
                                receiver, u"response",
                                buffer);
                            in2.set_prop_public(
                                receiver, u"responseText",
                                in2.str(""));
                        } else if (response_type == u"json") {
                            Value parsed =
                                Value::make_null();
                            try {
                                if (in2.is_callable(json_parse)) {
                                    std::vector<Value> arguments{
                                        in2.str(widen(text))};
                                    parsed = in2.call(
                                        json_parse,
                                        Value::make_undefined(),
                                        arguments);
                                }
                            } catch (
                                const js::runtime::ThrowSignal&) {
                                parsed = Value::make_null();
                            }
                            in2.set_prop_public(
                                receiver, u"response",
                                parsed);
                            in2.set_prop_public(
                                receiver, u"responseText",
                                in2.str(""));
                        } else {
                            Value text_value =
                                in2.str(widen(text));
                            in2.set_prop_public(
                                receiver, u"responseText",
                                text_value);
                            in2.set_prop_public(
                                receiver, u"response",
                                text_value);
                        }
                        in2.set_prop_public(
                            receiver, u"readyState",
                            Value::make_int32(4));
                        dispatch_xhr_event(
                            in2, receiver,
                            u"readystatechange",
                            response.body.size(),
                            response.body.size());
                        dispatch_xhr_event(
                            in2, receiver, u"progress",
                            response.body.size(),
                            response.body.size());
                        dispatch_xhr_event(
                            in2, receiver, u"load",
                            response.body.size(),
                            response.body.size());
                        dispatch_xhr_event(
                            in2, receiver, u"loadend",
                            response.body.size(),
                            response.body.size());
                    };

                const bool async = in.to_bool(
                    in.get_prop_public(
                        receiver, u"%async%"));
                if (!async) {
                    complete();
                    return Value::make_undefined();
                }
                in.add_host_root(receiver);
                in.event_loop()->post_task(
                    [self, receiver,
                     complete = std::move(complete)]() mutable {
                        complete();
                        self->engine_.interpreter()
                            .remove_host_root(receiver);
                    });
                return Value::make_undefined();
            },
            1)));
    interp.global()->define(
        u"XMLHttpRequest", Value::make_heap_ptr(xhr_ctor));

    // ---- location (parsed from the current URL via the URL builtin) ----
    JSObject* location = interp.new_object();
    if (Value* urlctor = interp.global()->find(u"URL")) {
        std::vector<Value> ua{ interp.str(current_url_) };
        Value u = interp.construct(*urlctor, ua);
        if (u.is_heap_ptr() && u.as_heap_ptr()->kind == js::vm::HeapObject::kJSObject) {
            JSObject* uo = static_cast<JSObject*>(u.as_heap_ptr());
            for (const char16_t* k : {u"href", u"protocol", u"host", u"hostname", u"port",
                                      u"pathname", u"search", u"hash", u"origin"})
                location->set(k, uo->get(k));
        }
    }
    location->set(u"reload", Value::make_heap_ptr(interp.new_native(u"reload",
        [self](Interpreter&, Value, std::vector<Value>&) { self->reload(); return Value::make_undefined(); })));
    location->set(u"assign", Value::make_heap_ptr(interp.new_native(u"assign",
        [self](Interpreter& in, Value, std::vector<Value>& a) { if (!a.empty()) self->load_url(narrow(in.to_string(a[0]))); return Value::make_undefined(); })));
    location->set(u"replace", location->get(u"assign"));
    location->set(u"toString", Value::make_heap_ptr(interp.new_native(u"toString",
        [location](Interpreter&, Value, std::vector<Value>&) { return location->get(u"href"); })));
    interp.global()->define(u"location", Value::make_heap_ptr(location));
    if (Value* document = interp.global()->find(u"document")) {
        interp.set_prop_public(
            *document, u"URL", interp.str(current_url_));
        interp.set_prop_public(
            *document, u"documentURI", interp.str(current_url_));
        interp.set_prop_public(
            *document, u"baseURI", interp.str(current_url_));
        interp.set_prop_public(
            *document, u"referrer", interp.str(""));
        interp.set_prop_public(
            *document, u"characterSet", interp.str("UTF-8"));
        interp.set_prop_public(
            *document, u"charset", interp.str("UTF-8"));
        interp.set_prop_public(
            *document, u"inputEncoding", interp.str("UTF-8"));
        interp.set_prop_public(
            *document, u"contentType", interp.str("text/html"));
        interp.set_prop_public(
            *document, u"compatMode", interp.str("CSS1Compat"));
        interp.set_prop_public(
            *document, u"visibilityState", interp.str("visible"));
        interp.set_prop_public(
            *document, u"hidden", Value::make_bool(false));
        interp.set_prop_public(
            *document, u"location", Value::make_heap_ptr(location));
    }

    // ---- navigator ----
    JSObject* navigator = interp.new_object();
    navigator->set(u"userAgent", interp.str(std::string("Mozilla/5.0 (RiduxOS) Seage/1.0 Malibu/1.0")));
    navigator->set(u"language", interp.str(std::string("en-US")));
    navigator->set(u"platform", interp.str(std::string("Linux x86_64")));
    navigator->set(u"onLine", Value::make_bool(true));
    navigator->set(u"hardwareConcurrency", Value::make_int32(4));
    navigator->set(u"maxTouchPoints", Value::make_int32(0));
    interp.global()->define(u"navigator", Value::make_heap_ptr(navigator));

    // ---- history (wired to the view's back/forward stack) ----
    JSObject* history = interp.new_object();
    history->set(u"length", Value::make_int32(1));
    history->set(u"state", Value::make_null());
    history->set(u"back", Value::make_heap_ptr(interp.new_native(u"back",
        [self](Interpreter&, Value, std::vector<Value>&) { self->go_back(); return Value::make_undefined(); })));
    history->set(u"forward", Value::make_heap_ptr(interp.new_native(u"forward",
        [self](Interpreter&, Value, std::vector<Value>&) { self->go_forward(); return Value::make_undefined(); })));
    history->set(u"go", Value::make_heap_ptr(interp.new_native(u"go",
        [self](Interpreter& in, Value, std::vector<Value>& a) { int n = a.empty() ? 0 : in.to_int32(a[0]); if (n < 0) self->go_back(); else if (n > 0) self->go_forward(); return Value::make_undefined(); })));
    // pushState/replaceState update history.state (SPA routing); no reload.
    auto state_setter = [history](Interpreter& in, Value, std::vector<Value>& a) {
        history->set(u"state", a.empty() ? Value::make_null() : a[0]); (void)in; return Value::make_undefined();
    };
    history->set(u"pushState", Value::make_heap_ptr(interp.new_native(u"pushState", state_setter)));
    history->set(u"replaceState", Value::make_heap_ptr(interp.new_native(u"replaceState", state_setter)));
    interp.global()->define(u"history", Value::make_heap_ptr(history));

    // ---- window / self alias the global object; expose the platform surface ----
    if (Value* gt = interp.global()->find(u"globalThis")) {
        if (gt->is_heap_ptr() && gt->as_heap_ptr()->kind == js::vm::HeapObject::kJSObject) {
            JSObject* win = static_cast<JSObject*>(gt->as_heap_ptr());
            Value window_ctor_value = win->get(u"Window");
            JSObject* window_proto = nullptr;
            if (window_ctor_value.is_heap_ptr() &&
                window_ctor_value.as_heap_ptr()->kind ==
                    js::vm::HeapObject::kJSFunction) {
                Value existing_proto =
                    static_cast<JSFunction*>(
                        window_ctor_value.as_heap_ptr())
                        ->get(u"prototype");
                if (existing_proto.is_heap_ptr() &&
                    existing_proto.as_heap_ptr()->kind ==
                        js::vm::HeapObject::kJSObject)
                    window_proto = static_cast<JSObject*>(
                        existing_proto.as_heap_ptr());
            }
            if (!window_proto) {
                window_proto = interp.new_object();
                window_proto->proto = win->proto;
                JSFunction* window_ctor = interp.new_native(
                    u"Window",
                    [](Interpreter& in, Value,
                       std::vector<Value>&) -> Value {
                        in.throw_error(u"TypeError",
                                       u"Illegal constructor");
                    });
                window_ctor->set(
                    u"prototype",
                    Value::make_heap_ptr(window_proto), false);
                window_proto->set(
                    u"constructor",
                    Value::make_heap_ptr(window_ctor), false);
                win->set(u"Window",
                         Value::make_heap_ptr(window_ctor));
            }
            win->proto = window_proto;
            win->set(u"location", Value::make_heap_ptr(location));
            win->set(u"navigator", Value::make_heap_ptr(navigator));
            win->set(u"history", Value::make_heap_ptr(history));
            win->set(u"self", *gt);
            win->set(u"window", *gt);
            // A top-level browsing context: parent/top/frames refer to itself,
            // no opener. (testharness.js walks window.parent, so these must exist.)
            win->set(u"parent", *gt);
            win->set(u"top", *gt);
            win->set(u"frames", *gt);
            win->set(u"opener", Value::make_null());
            win->set(u"length", Value::make_int32(0));
            // Window-level event registration: listeners are collected per type in
            // a hidden `__winEvents` object (kept alive via the global scope) and
            // fired by View::fire_window_event after the document's scripts run —
            // so `window.addEventListener('load'|'DOMContentLoaded', ...)` works.
            JSObject* winEvents = interp.new_object();
            interp.global()->define(u"__winEvents", Value::make_heap_ptr(winEvents));
            JSFunction* addEL = interp.new_native(u"addEventListener",
                [winEvents](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::make_undefined();
                    std::u16string type = in.to_string(a[0]);
                    Value arrv = winEvents->get(type);
                    JSArray* arr;
                    if (arrv.is_heap_ptr() && arrv.as_heap_ptr()->kind == js::vm::HeapObject::kJSArray)
                        arr = static_cast<JSArray*>(arrv.as_heap_ptr());
                    else { arr = in.new_array(); winEvents->set(type, Value::make_heap_ptr(arr)); }
                    arr->elements.push_back(a[1]);
                    return Value::make_undefined();
                });
            JSFunction* removeEL = interp.new_native(u"removeEventListener",
                [winEvents](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::make_undefined();
                    Value arrv = winEvents->get(in.to_string(a[0]));
                    if (arrv.is_heap_ptr() && arrv.as_heap_ptr()->kind == js::vm::HeapObject::kJSArray) {
                        auto& el = static_cast<JSArray*>(arrv.as_heap_ptr())->elements;
                        el.erase(std::remove_if(el.begin(), el.end(), [&](Value v){ return v == a[1]; }), el.end());
                    }
                    return Value::make_undefined();
                });
            win->set(u"addEventListener", Value::make_heap_ptr(addEL));
            win->set(u"removeEventListener", Value::make_heap_ptr(removeEL));
            win->set(u"dispatchEvent", Value::make_heap_ptr(interp.new_native(u"dispatchEvent",
                [](Interpreter&, Value, std::vector<Value>&) { return Value::make_bool(true); })));
            win->set(u"matchMedia", Value::make_heap_ptr(interp.new_native(u"matchMedia", [](Interpreter& in, Value, std::vector<Value>&) -> Value {
                JSObject* mq = in.new_object(); mq->set(u"matches", Value::make_bool(false));
                mq->set(u"addListener", Value::make_heap_ptr(in.new_native(u"addListener", [](Interpreter&, Value, std::vector<Value>&){ return Value::make_undefined(); })));
                mq->set(u"addEventListener", mq->get(u"addListener"));
                return Value::make_heap_ptr(mq); })));

            const auto performance_origin = std::chrono::steady_clock::now();
            const double time_origin_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            JSObject* navigation_entry = interp.new_object();
            navigation_entry->set(u"name", interp.str(current_url_));
            navigation_entry->set(u"entryType", interp.str("navigation"));
            navigation_entry->set(u"initiatorType", interp.str("navigation"));
            navigation_entry->set(u"type", interp.str("navigate"));
            navigation_entry->set(u"startTime", Value::make_double(0));
            navigation_entry->set(u"duration", Value::make_double(0));
            navigation_entry->set(u"fetchStart", Value::make_double(0));
            navigation_entry->set(u"domainLookupStart", Value::make_double(0));
            navigation_entry->set(u"domainLookupEnd", Value::make_double(0));
            navigation_entry->set(u"connectStart", Value::make_double(0));
            navigation_entry->set(u"secureConnectionStart", Value::make_double(0));
            navigation_entry->set(u"connectEnd", Value::make_double(0));
            navigation_entry->set(u"requestStart", Value::make_double(0));
            navigation_entry->set(u"responseStart", Value::make_double(1));
            navigation_entry->set(u"responseEnd", Value::make_double(1));
            navigation_entry->set(u"domInteractive", Value::make_double(1));
            navigation_entry->set(u"domContentLoadedEventStart", Value::make_double(1));
            navigation_entry->set(u"domContentLoadedEventEnd", Value::make_double(1));
            navigation_entry->set(u"domComplete", Value::make_double(1));
            navigation_entry->set(u"loadEventStart", Value::make_double(1));
            navigation_entry->set(u"loadEventEnd", Value::make_double(1));
            navigation_entry->set(u"redirectCount", Value::make_int32(0));
            navigation_entry->set(u"transferSize", Value::make_int32(0));
            navigation_entry->set(u"encodedBodySize", Value::make_int32(0));
            navigation_entry->set(u"decodedBodySize", Value::make_int32(0));
            navigation_entry->set(
                u"toJSON",
                Value::make_heap_ptr(interp.new_native(
                    u"toJSON",
                    [](Interpreter&, Value this_value,
                       std::vector<Value>&) { return this_value; })));

            JSObject* performance = interp.new_object();
            performance->set(u"timeOrigin", Value::make_double(time_origin_ms));
            performance->set(
                u"now",
                Value::make_heap_ptr(interp.new_native(
                    u"now",
                    [performance_origin](Interpreter&, Value,
                                         std::vector<Value>&) -> Value {
                        return Value::make_double(
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                performance_origin).count());
                    })));
            JSObject* timing = interp.new_object();
            const int64_t epoch = static_cast<int64_t>(time_origin_ms);
            for (const char16_t* field :
                 {u"navigationStart", u"fetchStart", u"domainLookupStart",
                  u"domainLookupEnd", u"connectStart", u"secureConnectionStart",
                  u"connectEnd", u"requestStart", u"responseStart",
                  u"responseEnd", u"domLoading", u"domInteractive",
                  u"domContentLoadedEventStart", u"domContentLoadedEventEnd",
                  u"domComplete", u"loadEventStart", u"loadEventEnd",
                  u"unloadEventStart", u"unloadEventEnd", u"redirectStart",
                  u"redirectEnd"})
                timing->set(field, Value::make_double(static_cast<double>(epoch)));
            performance->set(u"timing", Value::make_heap_ptr(timing));
            JSObject* navigation = interp.new_object();
            navigation->set(u"type", Value::make_int32(0));
            navigation->set(u"redirectCount", Value::make_int32(0));
            performance->set(u"navigation", Value::make_heap_ptr(navigation));
            auto performance_entries =
                [navigation_entry](Interpreter& in, bool include_navigation) {
                    JSArray* entries = in.new_array();
                    if (include_navigation)
                        entries->elements.push_back(
                            Value::make_heap_ptr(navigation_entry));
                    return Value::make_heap_ptr(entries);
                };
            performance->set(
                u"getEntries",
                Value::make_heap_ptr(interp.new_native(
                    u"getEntries",
                    [performance_entries](Interpreter& in, Value,
                                          std::vector<Value>&) {
                        return performance_entries(in, true);
                    })));
            performance->set(
                u"getEntriesByType",
                Value::make_heap_ptr(interp.new_native(
                    u"getEntriesByType",
                    [performance_entries](Interpreter& in, Value,
                                          std::vector<Value>& arguments) {
                        bool navigation =
                            !arguments.empty() &&
                            in.to_string(arguments[0]) == u"navigation";
                        return performance_entries(in, navigation);
                    })));
            performance->set(
                u"getEntriesByName",
                Value::make_heap_ptr(interp.new_native(
                    u"getEntriesByName",
                    [performance_entries, navigation_entry](
                        Interpreter& in, Value,
                        std::vector<Value>& arguments) {
                        const std::u16string entry_name =
                            in.to_string(navigation_entry->get(u"name"));
                        bool match = !arguments.empty() &&
                            in.to_string(arguments[0]) == entry_name;
                        return performance_entries(in, match);
                    })));
            auto perf_noop = [](Interpreter&, Value,
                                std::vector<Value>&) {
                return Value::make_undefined();
            };
            for (const char16_t* method :
                 {u"mark", u"measure", u"clearMarks", u"clearMeasures",
                  u"clearResourceTimings", u"setResourceTimingBufferSize"})
                performance->set(
                    method, Value::make_heap_ptr(
                                interp.new_native(method, perf_noop)));
            win->set(u"performance", Value::make_heap_ptr(performance));

            JSObject* css_namespace = interp.new_object();
            css_namespace->set(
                u"supports",
                Value::make_heap_ptr(interp.new_native(
                    u"supports",
                    [](Interpreter& in, Value,
                       std::vector<Value>& arguments) -> Value {
                        if (arguments.empty())
                            return Value::make_bool(false);
                        if (arguments.size() == 1)
                            return Value::make_bool(css::supports_condition(
                                in.to_string(arguments[0])));
                        return Value::make_bool(css::supports_property_value(
                            in.to_string(arguments[0]),
                            in.to_string(arguments[1])));
                    },
                    1)));
            css_namespace->set(
                u"escape",
                Value::make_heap_ptr(interp.new_native(
                    u"escape",
                    [](Interpreter& in, Value,
                       std::vector<Value>& arguments) -> Value {
                        if (arguments.empty())
                            in.throw_error(
                                u"TypeError",
                                u"CSS.escape requires one argument");
                        return in.str(escape_css_identifier(
                            in.to_string(arguments[0])));
                    },
                    1)));
            win->set(u"CSS", Value::make_heap_ptr(css_namespace));

            // Observers (MutationObserver/IntersectionObserver/ResizeObserver):
            // constructible no-throw objects so SPA init code runs. (Records are
            // not yet delivered; the host re-renders after DOM/scroll changes.)
            auto observer_ctor = [&interp](const char16_t* nm) {
                return Value::make_heap_ptr(interp.new_native(nm,
                    [](Interpreter& in, Value, std::vector<Value>&) -> Value {
                        JSObject* o = in.new_object();
                        auto noop = [](Interpreter&, Value, std::vector<Value>&) { return Value::make_undefined(); };
                        o->set(u"observe", Value::make_heap_ptr(in.new_native(u"observe", noop)));
                        o->set(u"unobserve", Value::make_heap_ptr(in.new_native(u"unobserve", noop)));
                        o->set(u"disconnect", Value::make_heap_ptr(in.new_native(u"disconnect", noop)));
                        o->set(u"takeRecords", Value::make_heap_ptr(in.new_native(u"takeRecords",
                            [](Interpreter& i2, Value, std::vector<Value>&) { return Value::make_heap_ptr(i2.new_array()); })));
                        return Value::make_heap_ptr(o);
                    }));
            };
            win->set(u"MutationObserver", observer_ctor(u"MutationObserver"));
            win->set(u"IntersectionObserver", observer_ctor(u"IntersectionObserver"));
            win->set(u"ResizeObserver", observer_ctor(u"ResizeObserver"));
            Value performance_observer =
                observer_ctor(u"PerformanceObserver");
            if (performance_observer.is_heap_ptr()) {
                auto* constructor = static_cast<JSFunction*>(
                    performance_observer.as_heap_ptr());
                JSArray* supported = interp.new_array();
                for (const char* type :
                     {"navigation", "resource", "mark", "measure",
                      "longtask", "event"})
                    supported->elements.push_back(interp.str(type));
                constructor->set(
                    u"supportedEntryTypes",
                    Value::make_heap_ptr(supported));
            }
            win->set(u"PerformanceObserver", performance_observer);
            // CustomEvent(type, {detail, bubbles}) — Event-like with a detail field.
            win->set(u"CustomEvent", Value::make_heap_ptr(interp.new_native(u"CustomEvent",
                [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    JSObject* e = in.new_object();
                    e->set(u"type", a.empty() ? in.str("") : a[0]);
                    e->set(u"bubbles", Value::make_bool(false));
                    e->set(u"detail", Value::make_undefined());
                    if (a.size() >= 2 && a[1].is_heap_ptr()) {
                        e->set(u"detail", in.get_prop_public(a[1], u"detail"));
                        Value bv = in.get_prop_public(a[1], u"bubbles");
                        e->set(u"bubbles", Value::make_bool(bv.is_bool() ? bv.as_bool() : (!bv.is_undefined() && !bv.is_null())));
                    }
                    auto noop = [](Interpreter&, Value, std::vector<Value>&) { return Value::make_undefined(); };
                    e->set(u"preventDefault", Value::make_heap_ptr(in.new_native(u"preventDefault", noop)));
                    e->set(u"stopPropagation", Value::make_heap_ptr(in.new_native(u"stopPropagation", noop)));
                    return Value::make_heap_ptr(e);
                })));

            // getComputedStyle(el): a CSSStyleDeclaration of resolved values.
            win->set(u"getComputedStyle", Value::make_heap_ptr(interp.new_native(u"getComputedStyle",
                [this](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    JSObject* o = in.new_object();
                    malibu::NodeHandle n = a.empty() ? malibu::NodeHandle::null_handle() : binding_->node_of(a[0]);
                    auto* core = doc_->core(n);
                    const css::ComputedStyle* cs = core ? core->computed_style : nullptr;
                    auto put = [&](const char16_t* camel, const char16_t* kebab, const std::string& v) {
                        Value sv = in.str(v); o->set(camel, sv); o->set(kebab, sv);
                    };
                    auto px = [](float v) { char b[32]; std::snprintf(b, sizeof b, "%gpx", v); return std::string(b); };
                    auto rgb = [](css::Color c) { char b[48];
                        if (c.a >= 255) std::snprintf(b, sizeof b, "rgb(%d, %d, %d)", c.r, c.g, c.b);
                        else std::snprintf(b, sizeof b, "rgba(%d, %d, %d, %g)", c.r, c.g, c.b, c.a / 255.0);
                        return std::string(b); };
                    if (cs) {
                        std::string disp = "block";
                        switch (cs->display) {
                            case css::DisplayType::Inline: disp = "inline"; break;
                            case css::DisplayType::InlineBlock: disp = "inline-block"; break;
                            case css::DisplayType::Flex: case css::DisplayType::InlineFlex: disp = "flex"; break;
                            case css::DisplayType::Grid: case css::DisplayType::InlineGrid: disp = "grid"; break;
                            case css::DisplayType::ListItem: disp = "list-item"; break;
                            case css::DisplayType::None: disp = "none"; break;
                            default: disp = "block";
                        }
                        put(u"display", u"display", disp);
                        const char* pos = cs->position == css::PositionType::Relative ? "relative"
                                        : cs->position == css::PositionType::Absolute ? "absolute"
                                        : cs->position == css::PositionType::Fixed ? "fixed"
                                        : cs->position == css::PositionType::Sticky ? "sticky" : "static";
                        put(u"position", u"position", pos);
                        put(u"visibility", u"visibility", cs->visibility == css::VisibilityType::Hidden ? "hidden" : "visible");
                        put(u"color", u"color", rgb(cs->color));
                        put(u"backgroundColor", u"background-color", rgb(cs->background_color));
                        put(u"fontSize", u"font-size", px(cs->font_size));
                        put(u"fontWeight", u"font-weight", cs->font_weight == css::FontWeight::Bold ? "700" : "400");
                        put(u"lineHeight", u"line-height", px(cs->line_height * cs->font_size));
                        { char b[16]; std::snprintf(b, sizeof b, "%g", cs->opacity); put(u"opacity", u"opacity", b); }
                        put(u"textAlign", u"text-align", cs->text_align == css::TextAlign::Center ? "center"
                                        : cs->text_align == css::TextAlign::Right ? "right" : "left");
                    }
                    if (auto* b = layout_.box_for_node(n)) {
                        put(u"width", u"width", px(b->width));
                        put(u"height", u"height", px(b->height));
                    }
                    o->set(u"getPropertyValue", Value::make_heap_ptr(in.new_native(u"getPropertyValue",
                        [o](Interpreter& i2, Value, std::vector<Value>& ar) -> Value {
                            if (ar.empty()) return i2.str("");
                            Value v = o->get(i2.to_string(ar[0]));
                            return v.is_undefined() ? i2.str("") : v;
                        })));
                    return Value::make_heap_ptr(o);
                })));
        }
        interp.global()->define(u"window", *gt);
        interp.global()->define(u"self", *gt);
    }
    install_worker_globals();
    install_wasm_globals();

    // DOM interface objects + constants + document.implementation. Tons of WPT
    // tests (and real pages) reference these for node-type constants, instanceof
    // checks, and document.implementation.* — without them tests error before any
    // assertion. Defined in JS as a prelude so they exist on every document.
    engine_.evaluate(R"JS((function(g){
      var NC={ELEMENT_NODE:1,ATTRIBUTE_NODE:2,TEXT_NODE:3,CDATA_SECTION_NODE:4,ENTITY_REFERENCE_NODE:5,ENTITY_NODE:6,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11,NOTATION_NODE:12,DOCUMENT_POSITION_DISCONNECTED:1,DOCUMENT_POSITION_PRECEDING:2,DOCUMENT_POSITION_FOLLOWING:4,DOCUMENT_POSITION_CONTAINS:8,DOCUMENT_POSITION_CONTAINED_BY:16,DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC:32};
      function Node(){} for(var k in NC) Node[k]=NC[k]; Node.__domInterface='Node'; g.Node=Node;
      var names=['Element','Document','HTMLDocument','XMLDocument','DocumentFragment','CharacterData','Text','Comment','CDATASection','ProcessingInstruction','Attr','NodeList','HTMLCollection','NamedNodeMap','DocumentType','DOMImplementation','HTMLElement','HTMLDivElement','HTMLSpanElement','HTMLInputElement','HTMLAnchorElement','HTMLImageElement','HTMLButtonElement','HTMLParagraphElement','HTMLScriptElement','HTMLFormElement','HTMLSelectElement','HTMLTextAreaElement','HTMLTemplateElement','HTMLIFrameElement','HTMLLinkElement','HTMLStyleElement','HTMLCanvasElement','HTMLUnknownElement','HTMLMediaElement','HTMLAudioElement','HTMLVideoElement','MediaError','SVGElement','Event','UIEvent','MouseEvent','KeyboardEvent','EventTarget','DOMTokenList','DOMException','DOMStringMap','ShadowRoot','Range','StaticRange','AbstractRange','NodeIterator','TreeWalker','XMLSerializer','Animation','KeyframeEffect','DocumentTimeline','IntersectionObserverEntry'];
      for(var i=0;i<names.length;i++){ if(!g[names[i]]) g[names[i]]=function(){}; g[names[i]].__domInterface=names[i]; }
      Element.prototype=Object.create(Node.prototype);Element.prototype.constructor=Element;
      HTMLElement.prototype=Object.create(Element.prototype);HTMLElement.prototype.constructor=HTMLElement;
      if(!Element.prototype.animate) Element.prototype.animate=function(){};
      if(!Element.prototype.getAnimations) Element.prototype.getAnimations=function(){return[];};
      var htmlInterfaces=['HTMLDivElement','HTMLSpanElement','HTMLInputElement','HTMLAnchorElement','HTMLImageElement','HTMLButtonElement','HTMLParagraphElement','HTMLScriptElement','HTMLFormElement','HTMLSelectElement','HTMLTextAreaElement','HTMLTemplateElement','HTMLIFrameElement','HTMLLinkElement','HTMLStyleElement','HTMLCanvasElement','HTMLUnknownElement'];
      for(var hi=0;hi<htmlInterfaces.length;hi++){
        var htmlCtor=g[htmlInterfaces[hi]];
        htmlCtor.prototype=Object.create(HTMLElement.prototype);
        htmlCtor.prototype.constructor=htmlCtor;
      }
      HTMLScriptElement.prototype.noModule=false;
      IntersectionObserverEntry.prototype.intersectionRatio=0;
      HTMLMediaElement.prototype=Object.create(HTMLElement.prototype);HTMLMediaElement.prototype.constructor=HTMLMediaElement;
      HTMLAudioElement.prototype=Object.create(HTMLMediaElement.prototype);HTMLAudioElement.prototype.constructor=HTMLAudioElement;
      HTMLVideoElement.prototype=Object.create(HTMLMediaElement.prototype);HTMLVideoElement.prototype.constructor=HTMLVideoElement;
      if(g.Audio){g.Audio.prototype=HTMLAudioElement.prototype;HTMLAudioElement.prototype.constructor=g.Audio;g.Audio.__domInterface='HTMLAudioElement';}
      if(g.Image){g.Image.prototype=HTMLImageElement.prototype;HTMLImageElement.prototype.constructor=g.Image;g.Image.__domInterface='HTMLImageElement';}
      var mediaConstants={NETWORK_EMPTY:0,NETWORK_IDLE:1,NETWORK_LOADING:2,NETWORK_NO_SOURCE:3,HAVE_NOTHING:0,HAVE_METADATA:1,HAVE_CURRENT_DATA:2,HAVE_FUTURE_DATA:3,HAVE_ENOUGH_DATA:4};
      for(var mk in mediaConstants){HTMLMediaElement[mk]=mediaConstants[mk];HTMLMediaElement.prototype[mk]=mediaConstants[mk];}
      MediaError.MEDIA_ERR_ABORTED=1;MediaError.MEDIA_ERR_NETWORK=2;MediaError.MEDIA_ERR_DECODE=3;MediaError.MEDIA_ERR_SRC_NOT_SUPPORTED=4;
      function NodeFilter(){} NodeFilter.SHOW_ALL=0xFFFFFFFF;NodeFilter.SHOW_ELEMENT=1;NodeFilter.SHOW_ATTRIBUTE=2;NodeFilter.SHOW_TEXT=4;NodeFilter.SHOW_CDATA_SECTION=8;NodeFilter.SHOW_ENTITY_REFERENCE=16;NodeFilter.SHOW_ENTITY=32;NodeFilter.SHOW_PROCESSING_INSTRUCTION=64;NodeFilter.SHOW_COMMENT=128;NodeFilter.SHOW_DOCUMENT=256;NodeFilter.SHOW_DOCUMENT_TYPE=512;NodeFilter.SHOW_DOCUMENT_FRAGMENT=1024;NodeFilter.SHOW_NOTATION=2048;NodeFilter.FILTER_ACCEPT=1;NodeFilter.FILTER_REJECT=2;NodeFilter.FILTER_SKIP=3; g.NodeFilter=NodeFilter;
      if(g.document && !g.document.implementation){
        g.document.implementation={hasFeature:function(){return true;},createHTMLDocument:function(){return g.document;},createDocument:function(){return g.document;},createDocumentType:function(n,p,s){return {name:n,publicId:p||'',systemId:s||'',nodeType:10,nodeName:n};}};
      }
      if(g.document){
        if(!g.document.timeline) g.document.timeline={currentTime:0};
        if(!g.document.getAnimations) g.document.getAnimations=function(){return[];};
      }
      if(!Element.prototype.attachShadow){
        Element.prototype.attachShadow=function(init){
          if(this.shadowRoot)return this.shadowRoot;
          var root=document.createDocumentFragment();
          root.host=this;
          root.mode=init&&init.mode||'open';
          root.delegatesFocus=!!(init&&init.delegatesFocus);
          this.shadowRoot=root;
          return root;
        };
      }
      if(!Node.prototype.getRootNode){
        Node.prototype.getRootNode=function(){
          var node=this;
          while(node&&node.parentNode)node=node.parentNode;
          return node;
        };
      }
      var definitions=Object.create(null);
      var waiting=Object.create(null);
      function copyDefinition(element,ctor){
        if(!element)return element;
        if(element.__ceDefinition===ctor){
          if(element.isConnected&&!element.__ceConnected&&typeof element.connectedCallback==='function'){
            element.__ceConnected=true;
            element.connectedCallback();
          }
          return element;
        }
        var chain=[],proto=ctor&&ctor.prototype;
        while(proto&&proto!==HTMLElement.prototype&&proto!==Element.prototype&&proto!==Object.prototype){
          chain.unshift(proto);
          proto=Object.getPrototypeOf(proto);
        }
        for(var i=0;i<chain.length;i++){
          var keys=Object.getOwnPropertyNames(chain[i]);
          for(var j=0;j<keys.length;j++){
            var key=keys[j];
            if(key==='constructor')continue;
            var descriptor=Object.getOwnPropertyDescriptor(chain[i],key);
            if(descriptor&&'value'in descriptor)element[key]=descriptor.value;
          }
        }
        try{
          var instance=new ctor();
          var own=Object.getOwnPropertyNames(instance);
          for(var k=0;k<own.length;k++)element[own[k]]=instance[own[k]];
        }catch(error){
          element.__ceConstructionError=error;
        }
        element.__ceDefinition=ctor;
        if(element.isConnected&&!element.__ceConnected&&typeof element.connectedCallback==='function'){
          element.__ceConnected=true;
          element.connectedCallback();
        }
        return element;
      }
      g.__malibuUpgradeCustomElement=function(element){
        if(!element||!element.tagName)return element;
        var ctor=definitions[String(element.tagName).toLowerCase()];
        return ctor?copyDefinition(element,ctor):element;
      };
      function CustomElementRegistry(){}
      CustomElementRegistry.prototype.define=function(name,ctor){
        name=String(name).toLowerCase();
        if(name.indexOf('-')<1)throw new DOMException('Invalid custom element name','SyntaxError');
        if(typeof ctor!=='function')throw new TypeError('Custom element constructor must be callable');
        if(definitions[name])throw new DOMException('Custom element already defined','NotSupportedError');
        definitions[name]=ctor;
        var elements=document.querySelectorAll(name);
        for(var i=0;i<elements.length;i++)copyDefinition(elements[i],ctor);
        if(waiting[name]){
          for(var j=0;j<waiting[name].length;j++)waiting[name][j](ctor);
          delete waiting[name];
        }
      };
      CustomElementRegistry.prototype.get=function(name){return definitions[String(name).toLowerCase()];};
      CustomElementRegistry.prototype.getName=function(ctor){
        for(var name in definitions)if(definitions[name]===ctor)return name;
      };
      CustomElementRegistry.prototype.whenDefined=function(name){
        name=String(name).toLowerCase();
        if(definitions[name])return Promise.resolve(definitions[name]);
        return new Promise(function(resolve){(waiting[name]||(waiting[name]=[])).push(resolve);});
      };
      CustomElementRegistry.prototype.upgrade=function(root){
        if(!root)return;
        g.__malibuUpgradeCustomElement(root);
        if(root.querySelectorAll){
          var all=root.querySelectorAll('*');
          for(var i=0;i<all.length;i++)g.__malibuUpgradeCustomElement(all[i]);
        }
      };
      g.CustomElementRegistry=CustomElementRegistry;
      g.customElements=new CustomElementRegistry();
    })(globalThis);)JS", "about:dom-prelude");

    // TreeWalker + NodeIterator (WHATWG DOM traversal), implemented in JS over the
    // engine's node properties — a self-contained WPT cluster (dom/traversal).
    engine_.evaluate(R"JS((function(g){
      function mkfilter(whatToShow, filter){
        return function(n){
          if(!((whatToShow >>> (n.nodeType-1)) & 1)) return 3;
          if(filter==null) return 1;
          return typeof filter==='function' ? filter(n) : filter.acceptNode(n);
        };
      }
      if(g.document) g.document.createTreeWalker=function(root,whatToShow,filter){
        if(root==null) throw new TypeError('root is required');
        whatToShow=(whatToShow===undefined||whatToShow===null)?0xFFFFFFFF:(whatToShow>>>0);
        var f=mkfilter(whatToShow,filter==null?null:filter);
        var tw={root:root,whatToShow:whatToShow,filter:(filter==null?null:filter),currentNode:root};
        function traverseChildren(first){
          var node = first? tw.currentNode.firstChild : tw.currentNode.lastChild;
          while(node){
            var r=f(node);
            if(r===1){ tw.currentNode=node; return node; }
            if(r!==2){ var c=first?node.firstChild:node.lastChild; if(c){ node=c; continue; } }
            while(node){ var sib=first?node.nextSibling:node.previousSibling; if(sib){ node=sib; break; } var p=node.parentNode; if(p==null||p===tw.root||p===tw.currentNode) return null; node=p; }
          }
          return null;
        }
        function traverseSiblings(next){
          var node=tw.currentNode; if(node===tw.root) return null;
          while(true){
            var sib=next?node.nextSibling:node.previousSibling;
            while(sib){ node=sib; var r=f(node); if(r===1){ tw.currentNode=node; return node; } sib=next?node.firstChild:node.lastChild; if(r===2||!sib) sib=next?node.nextSibling:node.previousSibling; }
            node=node.parentNode; if(node==null||node===tw.root) return null; if(f(node)===1) return null;
          }
        }
        tw.parentNode=function(){ var node=tw.currentNode; while(node!=null&&node!==tw.root){ node=node.parentNode; if(node!=null&&f(node)===1){ tw.currentNode=node; return node; } } return null; };
        tw.firstChild=function(){return traverseChildren(true);};
        tw.lastChild=function(){return traverseChildren(false);};
        tw.nextSibling=function(){return traverseSiblings(true);};
        tw.previousSibling=function(){return traverseSiblings(false);};
        tw.nextNode=function(){ var node=tw.currentNode, result=1;
          while(true){ while(result!==2&&node.firstChild){ node=node.firstChild; result=f(node); if(result===1){tw.currentNode=node;return node;} }
            var sib=null,temp=node; while(temp!=null){ if(temp===tw.root) return null; sib=temp.nextSibling; if(sib){break;} temp=temp.parentNode; }
            if(!sib) return null; node=sib; result=f(node); if(result===1){tw.currentNode=node;return node;} } };
        tw.previousNode=function(){ var node=tw.currentNode;
          while(node!==tw.root){ var sib=node.previousSibling; while(sib){ node=sib; var result=f(node); while(result!==2&&node.lastChild){ node=node.lastChild; result=f(node); } if(result===1){tw.currentNode=node;return node;} sib=node.previousSibling; }
            if(node===tw.root||node.parentNode==null) return null; node=node.parentNode; if(f(node)===1){tw.currentNode=node;return node;} } return null; };
        return tw;
      };
      if(g.document) g.document.createNodeIterator=function(root,whatToShow,filter){
        if(root==null) throw new TypeError('root is required');
        whatToShow=(whatToShow===undefined||whatToShow===null)?0xFFFFFFFF:(whatToShow>>>0);
        var f=mkfilter(whatToShow,filter==null?null:filter);
        function nextIn(node){ if(node.firstChild) return node.firstChild; while(node!=null&&node!==root){ if(node.nextSibling) return node.nextSibling; node=node.parentNode; } return null; }
        function prevIn(node){ if(node===root) return null; if(node.previousSibling){ node=node.previousSibling; while(node.lastChild) node=node.lastChild; return node; } return node.parentNode; }
        var ni={root:root,whatToShow:whatToShow,filter:(filter==null?null:filter),referenceNode:root,pointerBeforeReferenceNode:true};
        ni.nextNode=function(){ var node=ni.referenceNode, before=ni.pointerBeforeReferenceNode;
          while(true){ if(!before){ node=nextIn(node); if(node==null) return null; } before=false; if(f(node)===1){ ni.referenceNode=node; ni.pointerBeforeReferenceNode=false; return node; } } };
        ni.previousNode=function(){ var node=ni.referenceNode, before=ni.pointerBeforeReferenceNode;
          while(true){ if(before){ node=prevIn(node); if(node==null) return null; } before=true; if(f(node)===1){ ni.referenceNode=node; ni.pointerBeforeReferenceNode=true; return node; } } };
        ni.detach=function(){};
        return ni;
      };
    })(globalThis);)JS", "about:dom-traversal");
}

// ---------------------------------------------------------------------------
// WebAssembly JS API over MalibuWASM. Decoupled: the JS engine knows nothing
// about WASM; this binding (the integration layer) drives MalibuWASM and bridges
// values. Memory is synced JS<->WASM around each exported call.
// ---------------------------------------------------------------------------
void View::install_wasm_globals() {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArray;
    using js::runtime::JSFunction;
    using js::runtime::JSArrayBuffer;
    using js::runtime::JSTypedArray;
    namespace mw = malibu::wasm;
    struct WasmHostContext {
        mw::Instance* instance = nullptr;
        JSArrayBuffer* buffer = nullptr;
    };
    auto& interp = engine_.interpreter();
    View* self = this;

    // Extract the raw bytes from a Uint8Array / ArrayBuffer / Array argument.
    auto bytes_of = [](Interpreter& in, Value v) -> std::vector<uint8_t> {
        std::vector<uint8_t> out;
        if (!v.is_heap_ptr()) return out;
        auto* h = v.as_heap_ptr();
        if (h->kind == js::vm::HeapObject::kArrayBuffer) {
            auto* ab = static_cast<JSArrayBuffer*>(h);
            out = ab->data;
        } else if (h->kind == js::vm::HeapObject::kTypedArray) {
            auto* ta = static_cast<JSTypedArray*>(h);
            if (ta->buffer) out.assign(ta->buffer->data.begin() + ta->byte_offset,
                                       ta->buffer->data.begin() + ta->byte_offset + ta->byte_length());
        } else if (h->kind == js::vm::HeapObject::kJSArray) {
            for (Value e : static_cast<JSArray*>(h)->elements) out.push_back(static_cast<uint8_t>(in.to_number(e)));
        }
        return out;
    };
    auto js_from_wasm = [self](Interpreter&, mw::Value v) -> Value {
        switch (v.type) {
            case mw::ValType::I32: return Value::make_int32(v.i32);
            case mw::ValType::I64: return Value::make_double(static_cast<double>(v.i64));
            case mw::ValType::F32: return Value::make_double(static_cast<double>(v.f32));
            case mw::ValType::F64: return Value::make_double(v.f64);
            case mw::ValType::ExternRef:
                if (v.ref == 0) return Value::make_null();
                if (v.ref <= self->wasm_reference_values_.size())
                    return self->wasm_reference_values_[
                        static_cast<size_t>(v.ref - 1)];
                return Value::make_undefined();
            case mw::ValType::FuncRef:
                return v.ref == 0 ? Value::make_null()
                                  : Value::make_undefined();
            default: return Value::make_undefined();
        }
    };
    auto wasm_from_js = [self](Interpreter& in, Value v,
                               mw::ValType t) -> mw::Value {
        if (t == mw::ValType::ExternRef || t == mw::ValType::FuncRef) {
            if (v.is_null()) return mw::Value::Ref(t, 0);
            self->wasm_reference_values_.push_back(v);
            self->wasm_host_roots_.push_back(v);
            in.add_host_root(v);
            return mw::Value::Ref(
                t, self->wasm_reference_values_.size());
        }
        double d = in.to_number(v);
        switch (t) {
            case mw::ValType::I32: return mw::Value::I32(static_cast<int32_t>(static_cast<int64_t>(d)));
            case mw::ValType::I64: return mw::Value::I64(static_cast<int64_t>(d));
            case mw::ValType::F32: return mw::Value::F32(static_cast<float>(d));
            default: return mw::Value::F64(d);
        }
    };

    JSObject* WA = interp.new_object();

    // WebAssembly.Module(bytes): decode + store; wrapper holds the module index.
    JSObject* moduleProto = interp.new_object();
    auto make_module = [self, bytes_of, moduleProto](Interpreter& in, std::vector<Value>& a) -> Value {
        std::vector<uint8_t> bytes = bytes_of(in, a.empty() ? Value::make_undefined() : a[0]);
        auto dr = mw::decode(bytes.data(), bytes.size());
        if (!dr.ok()) in.throw_error(u"CompileError", std::u16string(u"WebAssembly.Module: ") +
                                     std::u16string(dr.error.begin(), dr.error.end()));
        self->wasm_modules_.push_back(std::move(dr.module));
        JSObject* obj = in.new_object();
        obj->proto = moduleProto;
        obj->set(u"%wasmModule%", Value::make_int32(static_cast<int32_t>(self->wasm_modules_.size() - 1)), false);
        return Value::make_heap_ptr(obj);
    };
    WA->set(u"Module", Value::make_heap_ptr(interp.new_native(u"Module",
        [make_module](Interpreter& in, Value, std::vector<Value>& a) { return make_module(in, a); }, 1)));

    // WebAssembly.Instance(module, importObject): instantiate + build exports.
    auto make_instance = [self, &interp, js_from_wasm, wasm_from_js](Interpreter& in, std::vector<Value>& a) -> Value {
        Value modv = a.empty() ? Value::make_undefined() : a[0];
        if (!modv.is_heap_ptr()) in.throw_error(u"TypeError", u"WebAssembly.Instance: bad module");
        Value idx = in.get_prop_public(modv, u"%wasmModule%");
        int mi = idx.is_int32() ? idx.as_int32() : -1;
        if (mi < 0 || mi >= static_cast<int>(self->wasm_modules_.size()))
            in.throw_error(u"TypeError", u"WebAssembly.Instance: not a Module");
        mw::Module* mod = self->wasm_modules_[mi].get();

        // Bind imported functions from importObject[module][name].
        Value importObj = a.size() > 1 ? a[1] : Value::make_undefined();
        std::vector<mw::HostFn> hosts;
        auto host_context = std::make_shared<WasmHostContext>();
        size_t import_index = 0;
        for (auto& imp : mod->func_imports) {
            const std::string import_label = imp.first + "." + imp.second;
            Value jsfn = Value::make_undefined();
            if (importObj.is_heap_ptr()) {
                Value ns = in.get_prop_public(importObj, std::u16string(imp.first.begin(), imp.first.end()));
                if (ns.is_heap_ptr()) jsfn = in.get_prop_public(ns, std::u16string(imp.second.begin(), imp.second.end()));
            }
            if (in.is_callable(jsfn)) {
                interp.add_host_root(jsfn);
                self->wasm_host_roots_.push_back(jsfn);
            }
            const mw::Func& imported = mod->funcs[import_index++];
            std::vector<mw::ValType> results =
                mod->types[imported.type_index].results;
            mw::HostFn fn = [self, jsfn, results, js_from_wasm,
                             wasm_from_js, host_context, import_label](
                                const std::vector<mw::Value>& args)
                -> std::vector<mw::Value> {
                auto& in2 = self->engine_.interpreter();
                const bool trace =
                    std::getenv("MALIBU_TRACE_WASM_IMPORTS") != nullptr;
                if (trace)
                    std::fprintf(stderr, "[wasm-import] call %s argc=%zu\n",
                                 import_label.c_str(), args.size());
                if (!in2.is_callable(jsfn)) {
                    if (trace)
                        std::fprintf(stderr,
                                     "[wasm-import] missing %s\n",
                                     import_label.c_str());
                    return {};
                }
                if (host_context->instance && host_context->buffer)
                    host_context->buffer->data =
                        host_context->instance->memory().data;
                std::vector<Value> jargs;
                for (auto& w : args) jargs.push_back(js_from_wasm(in2, w));
                Value returned;
                try {
                    returned =
                        in2.call(jsfn, Value::make_undefined(), jargs);
                } catch (...) {
                    if (host_context->instance &&
                        host_context->buffer)
                        host_context->instance->memory().data =
                            host_context->buffer->data;
                    throw;
                }
                if (host_context->instance && host_context->buffer)
                    host_context->instance->memory().data =
                        host_context->buffer->data;
                if (trace) {
                    const char* kind = "primitive";
                    if (returned.is_undefined()) kind = "undefined";
                    else if (returned.is_null()) kind = "null";
                    else if (returned.is_heap_ptr()) {
                        kind = returned.as_heap_ptr()->kind ==
                                       js::vm::HeapObject::kJSPromise
                                   ? "promise"
                                   : "object";
                    }
                    std::fprintf(stderr,
                                 "[wasm-import] return %s kind=%s results=%zu\n",
                                 import_label.c_str(), kind,
                                 results.size());
                }
                if (results.empty()) return {};
                if (results.size() == 1)
                    return {wasm_from_js(in2, returned, results[0])};

                std::vector<mw::Value> converted;
                converted.reserve(results.size());
                for (size_t i = 0; i < results.size(); ++i) {
                    std::string index = std::to_string(i);
                    Value element = in2.get_prop_public(
                        returned,
                        std::u16string(index.begin(), index.end()));
                    converted.push_back(
                        wasm_from_js(in2, element, results[i]));
                }
                return converted;
            };
            hosts.push_back(std::move(fn));
        }

        std::string err;
        auto inst = mw::instantiate(*mod, hosts, err);
        if (!inst) in.throw_error(u"LinkError", std::u16string(err.begin(), err.end()));
        self->wasm_instances_.push_back(std::move(inst));
        int ii = static_cast<int>(self->wasm_instances_.size() - 1);
        mw::Instance* instp = self->wasm_instances_[ii].get();
        host_context->instance = instp;

        // Linear memory: a JS ArrayBuffer that mirrors WASM memory. The exported
        // functions sync JS->WASM before a call and WASM->JS after, so a
        // TypedArray over `memory.buffer` reflects (and feeds) the WASM heap.
        JSArrayBuffer* memAb = nullptr;
        JSObject* memObj = nullptr;
        if (mod->has_memory) {
            memAb = interp.heap().alloc<JSArrayBuffer>();
            memAb->proto = nullptr;
            memAb->data = instp->memory().data;
            host_context->buffer = memAb;
            interp.push_root(Value::make_heap_ptr(memAb));  // (kept alive via exports below)
            memObj = in.new_object();
            memObj->set(u"buffer", Value::make_heap_ptr(memAb));
            memObj->set(u"grow", Value::make_heap_ptr(interp.new_native(
                u"grow",
                [self, ii](Interpreter& in2, Value receiver,
                           std::vector<Value>& ca) -> Value {
                    Value buffer_value = in2.get_prop_public(receiver, u"buffer");
                    if (!buffer_value.is_heap_ptr() ||
                        buffer_value.as_heap_ptr()->kind !=
                            js::vm::HeapObject::kArrayBuffer)
                        in2.throw_error(
                            u"TypeError",
                            u"WebAssembly.Memory.grow called on incompatible receiver");
                    auto* buffer =
                        static_cast<JSArrayBuffer*>(buffer_value.as_heap_ptr());
                    double requested =
                        ca.empty() ? 0 : in2.to_number(ca[0]);
                    if (!std::isfinite(requested) || requested < 0 ||
                        requested != std::floor(requested))
                        in2.throw_error(u"RangeError",
                                        u"WebAssembly.Memory.grow invalid delta");
                    auto& memory = self->wasm_instances_[ii]->memory();
                    uint64_t old_pages =
                        memory.data.size() / mw::Memory::kPageSize;
                    uint64_t new_pages =
                        old_pages + static_cast<uint64_t>(requested);
                    const mw::Module* module =
                        self->wasm_instances_[ii]->module();
                    if (module->mem_max_pages &&
                        new_pages > module->mem_max_pages)
                        in2.throw_error(
                            u"RangeError",
                            u"WebAssembly.Memory maximum size exceeded");
                    memory.data.resize(
                        static_cast<size_t>(new_pages) *
                            mw::Memory::kPageSize,
                        0);
                    buffer->data = memory.data;
                    return old_pages <=
                                   static_cast<uint64_t>(
                                       std::numeric_limits<int32_t>::max())
                               ? Value::make_int32(
                                     static_cast<int32_t>(old_pages))
                               : Value::make_double(
                                     static_cast<double>(old_pages));
                },
                1)));
        }

        JSObject* exports = in.new_object();
        auto make_wasm_function =
            [self, ii, memAb, js_from_wasm, wasm_from_js](
                Interpreter& in2, uint32_t function_index,
                const std::u16string& name) -> Value {
            mw::Instance* instance = self->wasm_instances_[ii].get();
            if (function_index >= instance->funcs().size())
                in2.throw_error(
                    u"RuntimeError",
                    u"WebAssembly function reference is out of range");
            const mw::FuncType& type =
                instance->module()->types[
                    instance->funcs()[function_index].type_index];
            std::vector<mw::ValType> params = type.params;
            JSFunction* native = in2.new_native(
                name,
                [self, ii, function_index, params, memAb, js_from_wasm,
                 wasm_from_js](Interpreter& call_interpreter, Value,
                               std::vector<Value>& arguments) -> Value {
                    mw::Instance* target =
                        self->wasm_instances_[ii].get();
                    if (memAb)
                        target->memory().data = memAb->data;
                    std::vector<mw::Value> wasm_arguments;
                    wasm_arguments.reserve(params.size());
                    for (size_t index = 0; index < params.size(); ++index) {
                        wasm_arguments.push_back(wasm_from_js(
                            call_interpreter,
                            index < arguments.size()
                                ? arguments[index]
                                : Value::make_undefined(),
                            params[index]));
                    }
                    std::string error;
                    auto returned = target->invoke(
                        function_index, wasm_arguments, error);
                    if (memAb) memAb->data = target->memory().data;
                    if (!returned)
                        call_interpreter.throw_error(
                            u"RuntimeError",
                            std::u16string(error.begin(), error.end()));
                    if (returned->empty())
                        return Value::make_undefined();
                    if (returned->size() == 1)
                        return js_from_wasm(
                            call_interpreter, (*returned)[0]);

                    JSArray* result = call_interpreter.new_array();
                    Value rooted = Value::make_heap_ptr(result);
                    call_interpreter.push_root(rooted);
                    for (const mw::Value& value : *returned)
                        result->append(
                            js_from_wasm(call_interpreter, value));
                    call_interpreter.pop_root();
                    return rooted;
                },
                static_cast<uint32_t>(params.size()));
            native->set(
                u"%wasmInstance%", Value::make_int32(ii), false);
            native->set(
                u"%wasmFunctionIndex%",
                Value::make_double(
                    static_cast<double>(function_index)),
                false);
            return Value::make_heap_ptr(native);
        };
        std::vector<Value> table_objects(
            mod->tables.size(), Value::make_undefined());
        auto table_object = [&](uint32_t index) -> Value {
            if (index >= mod->tables.size())
                in.throw_error(u"LinkError",
                               u"WebAssembly table export index is out of range");
            if (!table_objects[index].is_undefined())
                return table_objects[index];

            const mw::Table& table = mod->tables[index];
            const mw::TableStorage& storage =
                instp->tables()[index];
            JSObject* object = in.new_object();
            object->set(
                u"%wasmTable%",
                Value::make_int32(static_cast<int32_t>(index)), false);
            object->set(u"length",
                        Value::make_double(
                            static_cast<double>(
                                storage.elements.size())),
                        false);
            object->set(
                u"get",
                Value::make_heap_ptr(interp.new_native(
                    u"get",
                    [self, ii, js_from_wasm, make_wasm_function](
                        Interpreter& in2, Value receiver,
                        std::vector<Value>& ca) -> Value {
                        Value table_value =
                            in2.get_prop_public(receiver, u"%wasmTable%");
                        if (!table_value.is_int32())
                            in2.throw_error(
                                u"TypeError",
                                u"WebAssembly.Table.get called on incompatible receiver");
                        auto& table_storage =
                            self->wasm_instances_[ii]->tables()[
                                static_cast<size_t>(
                                    table_value.as_int32())];
                        double requested =
                            ca.empty() ? 0 : in2.to_number(ca[0]);
                        if (!std::isfinite(requested) || requested < 0 ||
                            requested != std::floor(requested) ||
                            requested >= table_storage.elements.size())
                            in2.throw_error(
                                u"RangeError",
                                u"WebAssembly.Table.get index is out of bounds");
                        const mw::Value element =
                            table_storage.elements[
                                static_cast<size_t>(requested)];
                        if (table_storage.element_type == 0x70) {
                            if (element.ref == 0)
                                return Value::make_null();
                            return make_wasm_function(
                                in2,
                                static_cast<uint32_t>(
                                    element.ref - 1),
                                u"");
                        }
                        return js_from_wasm(in2, element);
                    },
                    1)));
            object->set(
                u"set",
                Value::make_heap_ptr(interp.new_native(
                    u"set",
                    [self, ii, wasm_from_js](
                        Interpreter& in2, Value receiver,
                        std::vector<Value>& ca) -> Value {
                        Value table_value =
                            in2.get_prop_public(receiver, u"%wasmTable%");
                        if (!table_value.is_int32())
                            in2.throw_error(
                                u"TypeError",
                                u"WebAssembly.Table.set called on incompatible receiver");
                        auto& table_storage =
                            self->wasm_instances_[ii]->tables()[
                                static_cast<size_t>(
                                    table_value.as_int32())];
                        double requested =
                            ca.empty() ? 0 : in2.to_number(ca[0]);
                        if (!std::isfinite(requested) || requested < 0 ||
                            requested != std::floor(requested) ||
                            requested >= table_storage.elements.size())
                            in2.throw_error(
                                u"RangeError",
                                u"WebAssembly.Table.set index is out of bounds");
                        Value assigned =
                            ca.size() > 1 ? ca[1]
                                          : Value::make_null();
                        mw::Value converted;
                        if (table_storage.element_type == 0x70) {
                            if (assigned.is_null()) {
                                converted = mw::Value::Ref(
                                    mw::ValType::FuncRef, 0);
                            } else {
                                Value assigned_instance =
                                    in2.get_prop_public(
                                        assigned, u"%wasmInstance%");
                                Value assigned_function =
                                    in2.get_prop_public(
                                        assigned,
                                        u"%wasmFunctionIndex%");
                                if (!assigned_instance.is_int32() ||
                                    assigned_instance.as_int32() != ii ||
                                    (!assigned_function.is_int32() &&
                                     !assigned_function.is_double()))
                                    in2.throw_error(
                                        u"TypeError",
                                        u"WebAssembly.Table.set requires a function from the same instance");
                                converted = mw::Value::Ref(
                                    mw::ValType::FuncRef,
                                    static_cast<uint64_t>(
                                        in2.to_number(
                                            assigned_function)) +
                                        1);
                            }
                        } else {
                            converted = wasm_from_js(
                                in2, assigned,
                                mw::ValType::ExternRef);
                        }
                        table_storage.elements[
                            static_cast<size_t>(requested)] =
                            converted;
                        return Value::make_undefined();
                    },
                    2)));
            object->set(
                u"grow",
                Value::make_heap_ptr(interp.new_native(
                    u"grow",
                    [self, ii, wasm_from_js,
                     has_max = table.has_max,
                     maximum = table.max_size](
                        Interpreter& in2, Value receiver,
                        std::vector<Value>& ca) -> Value {
                        Value table_value =
                            in2.get_prop_public(receiver, u"%wasmTable%");
                        if (!table_value.is_int32())
                            in2.throw_error(
                                u"TypeError",
                                u"WebAssembly.Table.grow called on incompatible receiver");
                        auto& table_storage =
                            self->wasm_instances_[ii]->tables()[
                                static_cast<size_t>(
                                    table_value.as_int32())];
                        double requested =
                            ca.empty() ? 0 : in2.to_number(ca[0]);
                        if (!std::isfinite(requested) || requested < 0 ||
                            requested != std::floor(requested))
                            in2.throw_error(
                                u"RangeError",
                                u"WebAssembly.Table.grow invalid delta");
                        uint64_t old_size =
                            table_storage.elements.size();
                        uint64_t new_size =
                            old_size + static_cast<uint64_t>(requested);
                        if (has_max && new_size > maximum)
                            in2.throw_error(
                                u"RangeError",
                                u"WebAssembly.Table maximum size exceeded");
                        Value initial =
                            ca.size() > 1 ? ca[1] : Value::make_null();
                        mw::ValType type =
                            table_storage.element_type == 0x6F
                                ? mw::ValType::ExternRef
                                : mw::ValType::FuncRef;
                        table_storage.elements.resize(
                            static_cast<size_t>(new_size),
                            wasm_from_js(in2, initial, type));
                        in2.set_prop_public(
                            receiver, u"length",
                            Value::make_double(
                                static_cast<double>(new_size)));
                        return old_size <=
                                       static_cast<uint64_t>(
                                           std::numeric_limits<int32_t>::max())
                                   ? Value::make_int32(
                                         static_cast<int32_t>(old_size))
                                   : Value::make_double(
                                         static_cast<double>(old_size));
                    },
                    1)));
            Value result = Value::make_heap_ptr(object);
            table_objects[index] = result;
            return result;
        };
        for (auto& e : mod->exports) {
            std::u16string name(e.name.begin(), e.name.end());
            if (e.kind == 0) {
                exports->set(
                    name,
                    make_wasm_function(
                        in, e.index, name));
            } else if (e.kind == 1) {
                exports->set(name, table_object(e.index));
            } else if (e.kind == 2 && memObj) {
                exports->set(name, Value::make_heap_ptr(memObj));
            }
        }
        if (memAb) {
            interp.pop_root();
        }
        JSObject* obj = in.new_object();
        obj->set(u"exports", Value::make_heap_ptr(exports));
        return Value::make_heap_ptr(obj);
    };
    WA->set(u"Instance", Value::make_heap_ptr(interp.new_native(u"Instance",
        [make_instance](Interpreter& in, Value, std::vector<Value>& a) { return make_instance(in, a); }, 2)));

    // WebAssembly.compile / instantiate (promise forms).
    WA->set(u"compile", Value::make_heap_ptr(interp.new_native(u"compile",
        [make_module](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto* p = in.new_promise();
            try { in.resolve_promise(p, make_module(in, a)); }
            catch (js::runtime::ThrowSignal& s) { in.reject_promise(p, s.value); }
            return Value::make_heap_ptr(p); }, 1)));
    WA->set(u"instantiate", Value::make_heap_ptr(interp.new_native(u"instantiate",
        [make_module, make_instance](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto* p = in.new_promise();
            try {
                std::vector<Value> ma{ a.empty() ? Value::make_undefined() : a[0] };
                Value module = make_module(in, ma);
                std::vector<Value> ia{ module, a.size() > 1 ? a[1] : Value::make_undefined() };
                Value instance = make_instance(in, ia);
                JSObject* result = in.new_object();
                result->set(u"module", module);
                result->set(u"instance", instance);
                in.resolve_promise(p, Value::make_heap_ptr(result));
            } catch (js::runtime::ThrowSignal& s) { in.reject_promise(p, s.value); }
            return Value::make_heap_ptr(p); }, 2)));

    interp.global()->define(u"WebAssembly", Value::make_heap_ptr(WA));
}

// CanvasRenderingContext2D over MalibuCanvas. The context is a Proxy so that
// `ctx.fillStyle = "red"` / `ctx.lineWidth = 3` route to the surface (the same
// generalized host-object mechanism as el.style), while methods come from the
// target object.
js::runtime::Value View::make_canvas_context(malibu::NodeHandle node, const std::u16string& type) {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    namespace cv = malibu::canvas;
    auto& interp = engine_.interpreter();
    uint64_t key = (static_cast<uint64_t>(node.index) << 32) | node.generation;
    int cw = 300, ch = 150;
    if (auto a = tree_->get_attribute(node, u"width")) { int v = std::atoi(narrow(*a).c_str()); if (v > 0) cw = v; }
    if (auto a = tree_->get_attribute(node, u"height")) { int v = std::atoi(narrow(*a).c_str()); if (v > 0) ch = v; }

    if (type == u"webgl" || type == u"webgl2" || type == u"experimental-webgl") {
        auto git = gl_contexts_.find(key);
        if (git == gl_contexts_.end())
            git = gl_contexts_.emplace(key, std::make_shared<malibu::gl::Context>(cw, ch)).first;
        return make_webgl_context(git->second.get());
    }
    if (type != u"2d") return Value::make_null();
    auto it = canvases_.find(key);
    if (it == canvases_.end())
        it = canvases_.emplace(key, std::make_shared<cv::Canvas2D>(cw, ch)).first;
    cv::Canvas2D* c = it->second.get();

    JSObject* t = interp.new_object();
    auto m = [&](const char16_t* name, js::runtime::NativeFn fn) {
        t->set(name, Value::make_heap_ptr(interp.new_native(name, std::move(fn))));
    };
    auto num = [](Interpreter& in, std::vector<Value>& a, size_t i) { return i < a.size() ? in.to_number(a[i]) : 0.0; };
    m(u"fillRect",   [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->fill_rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"clearRect",  [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->clear_rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"strokeRect", [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->stroke_rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"beginPath",  [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->begin_path(); return Value::make_undefined(); });
    m(u"closePath",  [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->close_path(); return Value::make_undefined(); });
    m(u"moveTo",     [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->move_to(num(in,a,0),num(in,a,1)); return Value::make_undefined(); });
    m(u"lineTo",     [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->line_to(num(in,a,0),num(in,a,1)); return Value::make_undefined(); });
    m(u"rect",       [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"arc",        [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->arc(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3),num(in,a,4), a.size()>5 && in.to_bool(a[5])); return Value::make_undefined(); });
    m(u"fill",       [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->fill(); return Value::make_undefined(); });
    m(u"stroke",     [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->stroke(); return Value::make_undefined(); });
    m(u"save",       [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); });
    m(u"restore",    [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); });
    // current style values, mirrored on the target for reads.
    t->set(u"fillStyle", interp.str(std::string("#000000")));
    t->set(u"strokeStyle", interp.str(std::string("#000000")));
    t->set(u"lineWidth", Value::make_double(1.0));
    t->set(u"globalAlpha", Value::make_double(1.0));

    // Proxy: route style sets to the surface.
    JSObject* handler = interp.new_object();
    handler->set(u"set", Value::make_heap_ptr(interp.new_native(u"set",
        [c](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.size() < 3) return Value::make_bool(true);
            JSObject* target = a[0].is_heap_ptr() ? static_cast<JSObject*>(a[0].as_heap_ptr()) : nullptr;
            std::u16string k = in.to_string(a[1]);
            if (k == u"fillStyle")        c->set_fill_style(narrow(in.to_string(a[2])));
            else if (k == u"strokeStyle") c->set_stroke_style(narrow(in.to_string(a[2])));
            else if (k == u"lineWidth")   c->set_line_width(in.to_number(a[2]));
            else if (k == u"globalAlpha") c->set_global_alpha(in.to_number(a[2]));
            if (target) target->set(k, a[2]);  // mirror for reads
            return Value::make_bool(true);
        })));
    auto* px = interp.heap().alloc<js::runtime::JSProxy>();
    px->target = Value::make_heap_ptr(t);
    px->handler = Value::make_heap_ptr(handler);
    return Value::make_heap_ptr(px);
}

// WebGLRenderingContext over MalibuGL. GL object ids are JS numbers.
js::runtime::Value View::make_webgl_context(malibu::gl::Context* g) {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArrayBuffer;
    using js::runtime::JSTypedArray;
    auto& interp = engine_.interpreter();
    JSObject* gl = interp.new_object();
    auto C = [&](const char16_t* n, int v) { gl->set(n, Value::make_int32(v)); };
    C(u"VERTEX_SHADER", 0x8B31); C(u"FRAGMENT_SHADER", 0x8B30);
    C(u"ARRAY_BUFFER", 0x8892); C(u"ELEMENT_ARRAY_BUFFER", 0x8893);
    C(u"STATIC_DRAW", 0x88E4); C(u"DYNAMIC_DRAW", 0x88E8);
    C(u"FLOAT", 0x1406); C(u"UNSIGNED_BYTE", 0x1401); C(u"UNSIGNED_SHORT", 0x1403);
    C(u"COLOR_BUFFER_BIT", 0x4000); C(u"DEPTH_BUFFER_BIT", 0x0100);
    C(u"TRIANGLES", 4); C(u"TRIANGLE_STRIP", 5); C(u"TRIANGLE_FAN", 6); C(u"POINTS", 0); C(u"LINES", 1);
    C(u"COMPILE_STATUS", 0x8B81); C(u"LINK_STATUS", 0x8B82);
    C(u"DEPTH_TEST", 0x0B71); C(u"BLEND", 0x0BE2);
    C(u"TEXTURE_2D", 0x0DE1); C(u"TEXTURE0", 0x84C0); C(u"RGBA", 0x1908); C(u"RGB", 0x1907);
    C(u"NEAREST", 0x2600); C(u"LINEAR", 0x2601);
    C(u"TEXTURE_MAG_FILTER", 0x2800); C(u"TEXTURE_MIN_FILTER", 0x2801);
    C(u"TEXTURE_WRAP_S", 0x2802); C(u"TEXTURE_WRAP_T", 0x2803); C(u"CLAMP_TO_EDGE", 0x812F);

    auto bytes_of = [](Interpreter&, Value v, std::vector<uint8_t>& out) {
        if (!v.is_heap_ptr()) return;
        auto* h = v.as_heap_ptr();
        if (h->kind == js::vm::HeapObject::kTypedArray) { auto* ta = static_cast<JSTypedArray*>(h); if (ta->buffer) out.assign(ta->buffer->data.begin()+ta->byte_offset, ta->buffer->data.begin()+ta->byte_offset+ta->byte_length()); }
        else if (h->kind == js::vm::HeapObject::kArrayBuffer) out = static_cast<JSArrayBuffer*>(h)->data;
    };
    auto floats_of = [bytes_of](Interpreter& in, Value v) { std::vector<uint8_t> b; bytes_of(in, v, b); std::vector<float> f(b.size()/4); if (!b.empty()) std::memcpy(f.data(), b.data(), f.size()*4); return f; };
    auto I = [](Interpreter& in, std::vector<Value>& a, size_t i) { return i < a.size() ? static_cast<int>(in.to_number(a[i])) : 0; };
    auto F = [](Interpreter& in, std::vector<Value>& a, size_t i) { return i < a.size() ? static_cast<float>(in.to_number(a[i])) : 0.f; };
    auto m = [&](const char16_t* n, js::runtime::NativeFn fn) { gl->set(n, Value::make_heap_ptr(interp.new_native(n, std::move(fn)))); };

    m(u"createShader", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_int32((int)g->createShader(I(in,a,0))); });
    m(u"shaderSource", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->shaderSource(I(in,a,0), narrow(in.to_string(a.size()>1?a[1]:Value::make_undefined()))); return Value::make_undefined(); });
    m(u"compileShader", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->compileShader(I(in,a,0)); return Value::make_undefined(); });
    m(u"getShaderParameter", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_bool(g->getShaderParameter(I(in,a,0), I(in,a,1))); });
    m(u"getShaderInfoLog", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return in.str(g->getShaderInfoLog(I(in,a,0))); });
    m(u"createProgram", [g](Interpreter&, Value, std::vector<Value>&) { return Value::make_int32((int)g->createProgram()); });
    m(u"attachShader", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->attachShader(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"linkProgram", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->linkProgram(I(in,a,0)); return Value::make_undefined(); });
    m(u"getProgramParameter", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_bool(g->getProgramParameter(I(in,a,0), I(in,a,1))); });
    m(u"getProgramInfoLog", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return in.str(g->getProgramInfoLog(I(in,a,0))); });
    m(u"useProgram", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->useProgram(I(in,a,0)); return Value::make_undefined(); });
    m(u"createBuffer", [g](Interpreter&, Value, std::vector<Value>&) { return Value::make_int32((int)g->createBuffer()); });
    m(u"bindBuffer", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->bindBuffer(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"bufferData", [g,I,bytes_of](Interpreter& in, Value, std::vector<Value>& a) -> Value { std::vector<uint8_t> b; bytes_of(in, a.size()>1?a[1]:Value::make_undefined(), b); g->bufferData(I(in,a,0), b.data(), b.size()); return Value::make_undefined(); });
    m(u"getAttribLocation", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_int32(g->getAttribLocation(I(in,a,0), narrow(in.to_string(a.size()>1?a[1]:Value::make_undefined())))); });
    m(u"enableVertexAttribArray", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->enableVertexAttribArray(I(in,a,0)); return Value::make_undefined(); });
    m(u"vertexAttribPointer", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->vertexAttribPointer(I(in,a,0), I(in,a,1), I(in,a,2), a.size()>3&&in.to_bool(a[3]), I(in,a,4), I(in,a,5)); return Value::make_undefined(); });
    m(u"getUniformLocation", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_int32(g->getUniformLocation(I(in,a,0), narrow(in.to_string(a.size()>1?a[1]:Value::make_undefined())))); });
    m(u"uniform1f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform1f(I(in,a,0), F(in,a,1)); return Value::make_undefined(); });
    m(u"uniform2f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform2f(I(in,a,0), F(in,a,1), F(in,a,2)); return Value::make_undefined(); });
    m(u"uniform3f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform3f(I(in,a,0), F(in,a,1), F(in,a,2), F(in,a,3)); return Value::make_undefined(); });
    m(u"uniform4f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform4f(I(in,a,0), F(in,a,1), F(in,a,2), F(in,a,3), F(in,a,4)); return Value::make_undefined(); });
    m(u"uniform1i", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform1i(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"uniformMatrix4fv", [g,I,floats_of](Interpreter& in, Value, std::vector<Value>& a) -> Value { auto f = floats_of(in, a.size()>2?a[2]:Value::make_undefined()); if (f.size()>=16) g->uniformMatrix4fv(I(in,a,0), f.data()); return Value::make_undefined(); });
    m(u"clearColor", [g,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->clearColor(F(in,a,0), F(in,a,1), F(in,a,2), F(in,a,3)); return Value::make_undefined(); });
    m(u"clear", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->clear(I(in,a,0)); return Value::make_undefined(); });
    m(u"viewport", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->viewport(I(in,a,0), I(in,a,1), I(in,a,2), I(in,a,3)); return Value::make_undefined(); });
    m(u"drawArrays", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->drawArrays(I(in,a,0), I(in,a,1), I(in,a,2)); return Value::make_undefined(); });
    // textures
    m(u"createTexture", [g](Interpreter&, Value, std::vector<Value>&) { return Value::make_int32((int)g->createTexture()); });
    m(u"bindTexture", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->bindTexture(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"activeTexture", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->activeTexture(I(in,a,0)); return Value::make_undefined(); });
    m(u"texParameteri", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->texParameteri(I(in,a,1), I(in,a,2)); return Value::make_undefined(); });
    // texImage2D(target, level, internalformat, width, height, border, format, type, pixels)
    m(u"texImage2D", [g,I,bytes_of](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        std::vector<uint8_t> b;
        if (a.size() == 9) { int w = I(in,a,3), h = I(in,a,4); bytes_of(in, a[8], b); g->texImage2D(w, h, b.data(), b.size()); }
        else if (a.size() >= 6) { bytes_of(in, a[5], b); int side = (int)std::sqrt((double)(b.size()/4)); g->texImage2D(side, side, b.data(), b.size()); }
        return Value::make_undefined(); });
    // No-ops accepted by typical setup code.
    for (const char16_t* n : { u"enable", u"disable", u"blendFunc", u"depthFunc", u"deleteShader", u"deleteProgram", u"deleteBuffer", u"bindFramebuffer", u"activeTexture", u"pixelStorei", u"flush", u"finish" })
        m(n, [](Interpreter&, Value, std::vector<Value>&) { return Value::make_undefined(); });
    return Value::make_heap_ptr(gl);
}

void View::composite_canvases(render::Framebuffer& fb, float scroll_y) {
    const int sy_off = static_cast<int>(scroll_y);
    // Canvas backing-store pixels are mapped to the element's CSS content box.
    // The bitmap and CSS dimensions are independent (and commonly differ for
    // high-DPI canvases and low-resolution QR generators).
    auto blit = [&](malibu::NodeHandle node, const std::vector<uint8_t>& src, int cw, int ch) {
        layout::LayoutBox* box = layout_.box_for_node(node);
        if (!box || cw <= 0 || ch <= 0) return;
        int ox = static_cast<int>(box->x), oy = static_cast<int>(box->y) - sy_off;
        int bw = std::max(1, static_cast<int>(std::lround(
            box->width > 0 ? box->width : static_cast<float>(cw))));
        int bh = std::max(1, static_cast<int>(std::lround(
            box->height > 0 ? box->height : static_cast<float>(ch))));
        for (int y = 0; y < bh; ++y) {
            int dy = oy + y; if (dy < 0 || dy >= fb.height) continue;
            int sy = std::min(ch - 1, static_cast<int>(
                static_cast<int64_t>(y) * ch / bh));
            for (int x = 0; x < bw; ++x) {
                int dx = ox + x; if (dx < 0 || dx >= fb.width) continue;
                int sx = std::min(cw - 1, static_cast<int>(
                    static_cast<int64_t>(x) * cw / bw));
                size_t si = (static_cast<size_t>(sy) * cw + sx) * 4;
                uint8_t sa = src[si + 3];
                if (sa == 0) continue;
                size_t di = (static_cast<size_t>(dy) * fb.width + dx) * 4;
                double a = sa / 255.0;
                for (int k = 0; k < 3; ++k)
                    fb.rgba[di + k] = static_cast<uint8_t>(src[si + k] * a + fb.rgba[di + k] * (1 - a));
                fb.rgba[di + 3] = 255;
            }
        }
    };
    for (auto& [key, canvas] : canvases_) {
        malibu::NodeHandle node{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF)};
        blit(node, canvas->pixels(), canvas->width(), canvas->height());
    }
    for (auto& [key, ctx] : gl_contexts_) {
        malibu::NodeHandle node{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF)};
        blit(node, ctx->pixels(), ctx->width(), ctx->height());
    }
    // <img> bitmaps, scaled (nearest) to the element's content box.
    for (auto& [key, img] : images_) {
        if (!img.ok || img.width == 0 || img.height == 0) continue;
        malibu::NodeHandle node{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF)};
        layout::LayoutBox* box = layout_.box_for_node(node);
        if (!box) continue;
        int ox = static_cast<int>(box->x), oy = static_cast<int>(box->y) - sy_off;
        int bw = static_cast<int>(box->width > 0 ? box->width : img.width);
        int bh = static_cast<int>(box->height > 0 ? box->height : img.height);
        // object-fit: map box pixels → image pixels via a per-axis scale + offset.
        // `fill` (default) stretches each axis to the box; the others scale
        // uniformly and center, letterboxing (contain) or cropping (cover/none).
        const auto* st = box->style;
        auto fit = st ? st->object_fit : malibu::css::ObjectFit::Fill;
        double sxk = static_cast<double>(img.width) / bw;   // src px per dst px, x
        double syk = static_cast<double>(img.height) / bh;  // src px per dst px, y
        double offx = 0, offy = 0;                          // dst offset of the image
        if (fit != malibu::css::ObjectFit::Fill) {
            double scale;  // dst px per src px (uniform)
            double sc_contain = std::min(static_cast<double>(bw) / img.width, static_cast<double>(bh) / img.height);
            double sc_cover   = std::max(static_cast<double>(bw) / img.width, static_cast<double>(bh) / img.height);
            if (fit == malibu::css::ObjectFit::Contain)        scale = sc_contain;
            else if (fit == malibu::css::ObjectFit::Cover)     scale = sc_cover;
            else if (fit == malibu::css::ObjectFit::None)      scale = 1.0;
            else /* ScaleDown */                               scale = std::min(1.0, sc_contain);
            sxk = syk = 1.0 / scale;
            offx = (bw - img.width * scale) / 2.0;   // center (object-position: 50% 50%)
            offy = (bh - img.height * scale) / 2.0;
        }
        for (int y = 0; y < bh; ++y) {
            int dy = oy + y; if (dy < 0 || dy >= fb.height) continue;
            int sy = static_cast<int>((y - offy) * syk);
            if (sy < 0 || sy >= img.height) continue;   // letterbox / crop edge
            for (int x = 0; x < bw; ++x) {
                int dx = ox + x; if (dx < 0 || dx >= fb.width) continue;
                int sx = static_cast<int>((x - offx) * sxk);
                if (sx < 0 || sx >= img.width) continue;
                size_t si = (static_cast<size_t>(sy) * img.width + sx) * 4;
                uint8_t sa = img.rgba[si + 3];
                if (sa == 0) continue;
                size_t di = (static_cast<size_t>(dy) * fb.width + dx) * 4;
                double a = sa / 255.0;
                for (int k = 0; k < 3; ++k)
                    fb.rgba[di + k] = static_cast<uint8_t>(img.rgba[si + k] * a + fb.rgba[di + k] * (1 - a));
                fb.rgba[di + 3] = 255;
            }
        }
    }
}

void View::load_images(const malibu::html::ParsedDocument&,
                       bool reset_existing,
                       bool materialize_controls) {
    if (reset_existing) {
        images_.clear();
        layout_.clear_replaced_intrinsic_sizes();
    }
    std::vector<malibu::NodeHandle> imgs;
    tree_->query_selector_all(doc_->root(), u"img", imgs);
    if (!imgs.empty() && std::getenv("MALIBU_TRACE_SCRIPTS"))
        std::fprintf(stderr, "[load_images] found %zu <img> elements\n", imgs.size());
    // Picks the best URL from an <img>: real src, else the last (largest) srcset
    // candidate, else lazy-load data-src/data-srcset. Skips data: placeholders.
    auto pick_url = [&](malibu::NodeHandle node) -> std::u16string {
        auto srcset_last = [](const std::u16string& ss) -> std::u16string {
            std::u16string best; size_t i = 0;
            while (i < ss.size()) {
                while (i < ss.size() && (ss[i] == u' ' || ss[i] == u',' || ss[i] == u'\n' || ss[i] == u'\t')) ++i;
                size_t st = i; while (i < ss.size() && ss[i] != u' ' && ss[i] != u',') ++i;
                if (i > st) best = ss.substr(st, i - st);          // last candidate = largest
                while (i < ss.size() && ss[i] != u',') ++i;        // skip the descriptor
            }
            return best;
        };
        auto val = [&](const char16_t* n) -> std::u16string { auto a = tree_->get_attribute(node, n); return a ? *a : std::u16string(); };
        std::u16string src = val(u"src");
        if (!src.empty() && src.rfind(u"data:", 0) != 0) return src;
        std::u16string ss = val(u"srcset"); if (ss.empty()) ss = val(u"data-srcset");
        if (!ss.empty()) { auto u = srcset_last(ss); if (!u.empty()) return u; }
        std::u16string ds = val(u"data-src"); if (!ds.empty()) return ds;
        return src;
    };
    for (malibu::NodeHandle node : imgs) {
        uint64_t key =
            (static_cast<uint64_t>(node.index) << 32) | node.generation;
        if (!reset_existing && images_.contains(key)) continue;
        std::u16string url = pick_url(node);
        if (url.empty()) {
            if (std::getenv("MALIBU_TRACE_SCRIPTS"))
                std::fprintf(stderr, "[load_images] img node %u.%u has empty URL\n",
                            node.index, node.generation);
            continue;
        }
        const std::string resolved = resolve_url(url);
        if (std::getenv("MALIBU_TRACE_SCRIPTS"))
            std::fprintf(stderr, "[load_images] url='%s' resolved='%s'\n",
                        narrow(url).c_str(), resolved.c_str());
        if (url.rfind(u"data:", 0) == 0) {
            std::string data_url = narrow(url);
            size_t comma = data_url.find(',');
            if (comma != std::string::npos) {
                std::string metadata = data_url.substr(5, comma - 5);
                std::string payload = data_url.substr(comma + 1);
                bool base64 = metadata.size() >= 7 && metadata.rfind(";base64") == metadata.size() - 7;
                std::string decoded;
                decoded.reserve(payload.size());
                for (size_t i = 0; i < payload.size(); ++i) {
                    if (payload[i] == '%' && i + 2 < payload.size()) {
                        int high = hex_digit(payload[i + 1]);
                        int low = hex_digit(payload[i + 2]);
                        if (high >= 0 && low >= 0) {
                            decoded.push_back(static_cast<char>((high << 4) | low));
                            i += 2;
                            continue;
                        }
                    }
                    decoded.push_back(payload[i]);
                }
                if (base64) {
                    signed char table[256];
                    std::fill(std::begin(table), std::end(table), static_cast<signed char>(-1));
                    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    for (size_t i = 0; i < alphabet.size(); ++i)
                        table[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
                    std::string b64decoded;
                    b64decoded.reserve(decoded.size());
                    unsigned int bits = 0;
                    int bit_count = 0;
                    for (unsigned char c : decoded) {
                        if (c == '=') break;
                        int value = table[c];
                        if (value < 0) { if (std::isspace(c)) continue; break; }
                        bits = (bits << 6) | static_cast<unsigned int>(value);
                        bit_count += 6;
                        if (bit_count >= 8) {
                            bit_count -= 8;
                            b64decoded.push_back(static_cast<char>((bits >> bit_count) & 0xFF));
                        }
                    }
                    decoded = std::move(b64decoded);
                }
                malibu::image::DecodedImage img = malibu::image::decode_image(
                    reinterpret_cast<const uint8_t*>(decoded.data()), decoded.size());
                if (img.ok) {
                    layout_.set_replaced_intrinsic_size(
                        node, static_cast<float>(img.width), static_cast<float>(img.height));
                    images_[key] = std::move(img);
                    continue;
                }
            }
            record_diagnostic(LoadDiagnosticKind::Unsupported, resolved,
                              "data URL image decode failed");
            continue;
        }
        if (!fetch_handler_ && !request_handler_) {
            record_diagnostic(LoadDiagnosticKind::Resource, resolved,
                              "no resource loader is installed for image");
            continue;
        }
        network::FetchResponse resp;
        if (!perform_request(resolved, resp)) {
            record_diagnostic(LoadDiagnosticKind::Resource, resolved,
                              "failed to fetch image");
            continue;
        }
        if (is_http_error(resp)) {
            record_diagnostic(
                LoadDiagnosticKind::Resource, resolved,
                "image request returned HTTP " +
                    std::to_string(resp.status));
            continue;
        }
        const auto& body = resp.body;
        // SVG sniff (by extension/content) — vector, so it needs a target size.
        // Check if URL ends with .svg (not just contains .svg anywhere)
        bool is_svg = false;
        if (url.size() >= 4) {
            std::u16string_view end = std::u16string_view(url).substr(url.size() - 4);
            is_svg = (end == u".svg");
        }
        if (!is_svg && body.size() > 5) {
            std::string head(body.begin(), body.begin() + std::min<size_t>(body.size(), 256));
            is_svg = head.find("<svg") != std::string::npos || head.find("<?xml") != std::string::npos;
        }
        malibu::image::DecodedImage img;
        int wattr = 0, hattr = 0;
        if (auto a = tree_->get_attribute(node, u"width")) wattr = std::atoi(narrow(*a).c_str());
        if (auto a = tree_->get_attribute(node, u"height")) hattr = std::atoi(narrow(*a).c_str());
        if (is_svg) {
            int sw = wattr > 0 ? wattr : (hattr > 0 ? hattr : 64);
            int sh = hattr > 0 ? hattr : (wattr > 0 ? wattr : 64);
            img = malibu::image::decode_svg(body.data(), body.size(), sw, sh);
        } else {
            img = malibu::image::decode_image(body.data(), body.size());
        }
        if (!img.ok) {
            record_diagnostic(LoadDiagnosticKind::Resource, resolved,
                              "image format could not be decoded");
            continue;
        }
        if (std::getenv("MALIBU_TRACE_SCRIPTS"))
            std::fprintf(stderr, "[load_images] decoded %s: %dx%d ok=%d\n",
                        resolved.c_str(), img.width, img.height, (int)img.ok);
        layout_.set_replaced_intrinsic_size(
            node, static_cast<float>(img.width), static_cast<float>(img.height));
        images_[key] = std::move(img);
    }

    // Inline <svg> icons: serialize the subtree back to SVG text and rasterize.
    std::vector<malibu::NodeHandle> svgs;
    tree_->query_selector_all(doc_->root(), u"svg", svgs);
    std::function<std::string(malibu::NodeHandle)> serialize = [&](malibu::NodeHandle n) -> std::string {
        auto* c = doc_->core(n);
        if (!c) return "";
        if (c->node_type == malibu::dom::kTextNode) return narrow(c->text_content);
        if (c->node_type != malibu::dom::kElementNode) return "";
        std::string o = "<" + narrow(c->tag_name);
        std::string inline_style;
        for (auto& [k, v] : c->attributes) {
            if (k == u"style") inline_style = narrow(v);
            else o += " " + narrow(k) + "=\"" + narrow(v) + "\"";
        }
        if (c->computed_style && c->computed_style->svg_fill_specified) {
            const auto fill = c->computed_style->svg_fill;
            if (!inline_style.empty() && inline_style.back() != ';')
                inline_style.push_back(';');
            inline_style += "fill:rgba(" + std::to_string(fill.r) + "," +
                            std::to_string(fill.g) + "," +
                            std::to_string(fill.b) + "," +
                            std::to_string(fill.a / 255.0f) + ")";
        }
        if (!inline_style.empty()) o += " style=\"" + inline_style + "\"";
        o += ">";
        for (auto ch : c->children) o += serialize(ch);
        return o + "</" + narrow(c->tag_name) + ">";
    };
    for (malibu::NodeHandle node : svgs) {
        uint64_t key =
            (static_cast<uint64_t>(node.index) << 32) | node.generation;
        if (!reset_existing && images_.contains(key)) continue;
        float intrinsic_w = 0, intrinsic_h = 0, ratio = 0;
        if (auto a = tree_->get_attribute(node, u"width"))
            intrinsic_w = std::strtof(narrow(*a).c_str(), nullptr);
        if (auto a = tree_->get_attribute(node, u"height"))
            intrinsic_h = std::strtof(narrow(*a).c_str(), nullptr);
        auto view_box = tree_->get_attribute(node, u"viewbox");
        if (view_box) {
            std::istringstream values(narrow(*view_box));
            float x = 0, y = 0, width = 0, height = 0;
            if (values >> x >> y >> width >> height && width > 0 && height > 0)
                ratio = width / height;
        }
        if (ratio <= 0 && intrinsic_w > 0 && intrinsic_h > 0)
            ratio = intrinsic_w / intrinsic_h;
        if (intrinsic_w <= 0 && intrinsic_h > 0 && ratio > 0)
            intrinsic_w = intrinsic_h * ratio;
        if (intrinsic_h <= 0 && intrinsic_w > 0 && ratio > 0)
            intrinsic_h = intrinsic_w / ratio;
        if (intrinsic_w <= 0 && intrinsic_h <= 0) {
            intrinsic_w = ratio > 0 ? 150.0f * ratio : 300.0f;
            intrinsic_h = 150.0f;
        } else {
            if (intrinsic_w <= 0) intrinsic_w = 300.0f;
            if (intrinsic_h <= 0) intrinsic_h = 150.0f;
        }
        int sw = std::clamp(static_cast<int>(std::ceil(intrinsic_w)), 1, 2048);
        int sh = std::clamp(static_cast<int>(std::ceil(intrinsic_h)), 1, 2048);
        std::string svgtext = serialize(node);
        auto img = malibu::image::decode_svg(reinterpret_cast<const uint8_t*>(svgtext.data()), svgtext.size(), sw, sh);
        if (!img.ok) {
            record_diagnostic(LoadDiagnosticKind::Resource, current_url_,
                              "inline SVG could not be decoded");
            continue;
        }
        layout_.set_replaced_intrinsic_size(node, intrinsic_w, intrinsic_h);
        images_[key] = std::move(img);
    }

    replaced_content_dirty_ = false;
    if (!materialize_controls) return;

    // Form controls: give <input>/<textarea> a visible box and render their
    // value/placeholder text (so search boxes, buttons etc. appear).
    std::vector<malibu::NodeHandle> inputs;
    tree_->query_selector_all(doc_->root(), u"input", inputs);
    for (malibu::NodeHandle n : inputs) {
        std::u16string type = tree_->get_attribute(n, u"type").value_or(u"text");
        for (auto& ch : type) if (ch >= u'A' && ch <= u'Z') ch += 32;
        if (type == u"hidden") continue;
        std::string st;
        if (auto s = tree_->get_attribute(n, u"style")) st = narrow(*s) + ";";
        st += "display:inline-block;box-sizing:border-box;";
        if (type == u"checkbox" || type == u"radio") {
            st += "width:13px;height:13px;border:1px solid #888;background:#fff;";
        } else if (type == u"submit" || type == u"button" || type == u"reset") {
            st += "border:1px solid #767676;background:#efefef;padding:2px 10px;height:22px;color:#000;";
        } else {  // text/search/email/password/url/number...
            int size = 20; if (auto a = tree_->get_attribute(n, u"size")) { int v = std::atoi(narrow(*a).c_str()); if (v > 0) size = v; }
            st += "border:1px solid #767676;background:#fff;padding:2px 4px;height:22px;color:#000;width:" + std::to_string(size * 8 + 8) + "px;";
        }
        tree_->set_attribute(n, u"style", widen(st));
        // value / placeholder / button label as a text child.
        std::u16string txt = tree_->get_attribute(n, u"value").value_or(u"");
        bool placeholder = false;
        if (txt.empty()) { if (auto p = tree_->get_attribute(n, u"placeholder")) { txt = *p; placeholder = true; } }
        if (!txt.empty() && type != u"checkbox" && type != u"radio" && doc_->core(n) && doc_->core(n)->children.empty()) {
            if (placeholder) tree_->set_attribute(n, u"style", widen(st + "color:#757575;"));
            tree_->append_child(n, tree_->create_text_node(txt));
        }
    }

    // <select>: a native popup control shows only the selected option as a
    // single-line label (the option list is not in flow — UA CSS hides it).
    // Inject the selected (or first) option's text plus a dropdown caret.
    std::vector<malibu::NodeHandle> selects;
    tree_->query_selector_all(doc_->root(), u"select", selects);
    for (malibu::NodeHandle n : selects) {
        std::vector<malibu::NodeHandle> opts;
        tree_->query_selector_all(n, u"option", opts);
        // NB: a default-constructed NodeHandle is {0,0} = the document node, NOT
        // null (is_null() tests index==UINT32_MAX) — must seed with null_handle().
        malibu::NodeHandle chosen = malibu::NodeHandle::null_handle();
        for (malibu::NodeHandle o : opts) if (tree_->get_attribute(o, u"selected")) { chosen = o; break; }
        if (chosen.is_null() && !opts.empty()) chosen = opts[0];
        std::u16string label = chosen.is_null() ? u"" : tree_->text_content(chosen);
        // Collapse runs of whitespace so multi-line option text stays one line.
        std::u16string clean; bool sp = false;
        for (char16_t c : label) {
            bool ws = (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r');
            if (ws) { if (!clean.empty()) sp = true; } else { if (sp) clean.push_back(u' '); sp = false; clean.push_back(c); }
        }
        std::string st;
        if (auto s = tree_->get_attribute(n, u"style")) st = narrow(*s) + ";";
        st += "display:inline-block;box-sizing:border-box;border:1px solid #767676;background:#fff;"
              "padding:2px 20px 2px 6px;height:22px;color:#000;white-space:nowrap;overflow:hidden;";
        tree_->set_attribute(n, u"style", widen(st));
        tree_->append_child(n, tree_->create_text_node(clean + u" ▾"));
    }
}

void View::apply_styles() {
    css::CSSParser cssp;
    // Rebuild the resolver from scratch (cheap) so re-styling after script
    // mutations is correct.
    resolver_ = std::make_unique<css::StyleResolver>();
    resolver_->add_stylesheet(cssp.parse(css::user_agent_css()), css::Origin::UserAgent);
    for (const std::u16string& sheet : pending_stylesheets_)
        resolver_->add_stylesheet(cssp.parse(sheet), css::Origin::Author);
    resolver_->set_viewport(static_cast<float>(last_vw_), static_cast<float>(last_vh_));  // @media
    resolver_->resolve(*doc_);
}

std::string View::resolve_url(const std::u16string& ref16) const {
    std::string ref = narrow(ref16);
    // Trim leading/trailing ASCII whitespace (srcset/href can carry it).
    size_t a = ref.find_first_not_of(" \t\r\n"); size_t b = ref.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return current_url_;
    ref = ref.substr(a, b - a + 1);
    // Protocol-relative (//host/path): adopt the base URL's scheme.
    if (ref.rfind("//", 0) == 0) {
        std::string scheme = current_url_.rfind("http://", 0) == 0 ? "http:" : "https:";
        return scheme + ref;
    }
    // Absolute URL with any valid scheme, including data:, blob:, mailto:, ...
    const size_t colon = ref.find(':');
    const size_t first_separator = ref.find_first_of("/?#");
    if (colon != std::string::npos && colon > 0 &&
        (first_separator == std::string::npos ||
         colon < first_separator)) {
        bool valid_scheme =
            std::isalpha(static_cast<unsigned char>(ref[0])) != 0;
        for (size_t i = 1; valid_scheme && i < colon; ++i) {
            const unsigned char c =
                static_cast<unsigned char>(ref[i]);
            valid_scheme = std::isalnum(c) || c == '+' ||
                           c == '-' || c == '.';
        }
        if (valid_scheme) return ref;
    }
    // Root-relative (/foo): replace the path of the base URL's origin.
    if (ref[0] == '/') {
        auto s = current_url_.find("://");
        if (s != std::string::npos) {
            auto slash = current_url_.find('/', s + 3);
            std::string origin = slash == std::string::npos ? current_url_ : current_url_.substr(0, slash);
            return origin + ref;
        }
        return ref;  // e.g. file path root
    }
    // Document-relative: strip the base's last path segment.
    auto slash = current_url_.find_last_of('/');
    std::string dir = slash == std::string::npos ? current_url_ : current_url_.substr(0, slash + 1);
    return dir + ref;
}

void View::run_scripts(const std::vector<malibu::html::ScriptItem>& items) {
    const bool trace_scripts = std::getenv("MALIBU_TRACE_SCRIPTS") != nullptr;
    auto execute = [&](size_t index) {
        const auto& it = items[index];
        const std::string type = trim_ascii_lower(it.type);
        const bool module = type == "module";
        if (!module && it.no_module) return;
        if (!module && !is_classic_script_type(type)) {
            return;  // JSON, import maps and other data blocks are not JS.
        }

        std::string url = current_url_ + "#inline-script-" +
                          std::to_string(index + 1);
        js::Engine::EvalResult result;
        if (it.external) {
            url = resolve_url(it.src);
            if (it.src.empty()) {
                record_diagnostic(LoadDiagnosticKind::Resource, url,
                                  "script has an empty src attribute");
                return;
            }
            if (url.rfind("data:", 0) != 0 &&
                !fetch_handler_ && !request_handler_) {
                record_diagnostic(LoadDiagnosticKind::Resource, url,
                                  "no resource loader is installed");
                return;
            }
            std::string body;
            if (!load_script_source(url, body)) {
                record_diagnostic(LoadDiagnosticKind::Resource, url,
                                  "failed to fetch script");
                return;
            }
            if (trace_scripts)
                std::fprintf(stderr, "[script] start %s\n", url.c_str());
            set_import_meta(engine_, url);
            result = module ? engine_.evaluate_module(body, url)
                            : engine_.evaluate(body, url);
        } else {
            if (trace_scripts)
                std::fprintf(stderr, "[script] start %s\n", url.c_str());
            set_import_meta(engine_, url);
            result = module
                ? engine_.evaluate_module(narrow(it.code), url)
                : engine_.evaluate(narrow(it.code), url);
        }
        if (trace_scripts)
            std::fprintf(stderr, "[script] end %s (%s)\n", url.c_str(),
                         result.ok ? "ok" : "error");
        if (!result.ok) {
            record_diagnostic(LoadDiagnosticKind::Script, std::move(url),
                              result.error);
        }
    };

    // Parser-inserted classic scripts run while the document is parsed.
    // Modules are deferred by default and execute in document order after all
    // parser-inserted classic scripts have completed.
    for (size_t index = 0; index < items.size(); ++index) {
        if (trim_ascii_lower(items[index].type) != "module")
            execute(index);
    }
    for (size_t index = 0; index < items.size(); ++index) {
        if (trim_ascii_lower(items[index].type) == "module")
            execute(index);
    }
    run_tasks();  // settle page and worker tasks ready during load
}

void View::handle_dom_mutation(malibu::NodeHandle node) {
    layout_dirty_ = true;
    replaced_content_dirty_ = true;
    for (malibu::NodeHandle current = node; !current.is_null();
         current = tree_->parent_node(current)) {
        const dom::NodeCore* core = doc_->core(current);
        if (core && core->node_type == dom::kElementNode &&
            (core->tag_name == u"img" || core->tag_name == u"svg")) {
            const uint64_t key =
                (static_cast<uint64_t>(current.index) << 32) |
                current.generation;
            images_.erase(key);
            break;
        }
    }
    if (!node.is_null() && tree_->is_connected(node))
        prepare_connected_resource(node);
}

void View::prepare_connected_resource(malibu::NodeHandle node) {
    const dom::NodeCore* core = doc_->core(node);
    if (!core || core->node_type != dom::kElementNode) return;

    const std::u16string tag = core->tag_name;
    if (tag != u"script" && tag != u"style" && tag != u"link") return;
    const uint64_t key = webcall::encode_handle(node);
    if (!prepared_resource_nodes_.insert(key).second) return;

    if (tag == u"style") {
        pending_stylesheets_.push_back(tree_->text_content(node));
        layout_dirty_ = true;
        return;
    }

    auto attribute = [&](const char16_t* name) {
        return tree_->get_attribute(node, name).value_or(std::u16string());
    };
    if (tag == u"link") {
        std::u16string rel = attribute(u"rel");
        for (char16_t& c : rel)
            if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a';
        const std::u16string href = attribute(u"href");
        if (rel.find(u"stylesheet") == std::u16string::npos || href.empty())
            return;

        const std::string url = resolve_url(href);
        dom::Document* document = doc_.get();
        engine_.event_loop().post_task([this, document, node, url]() {
            if (doc_.get() != document) return;
            network::FetchResponse response;
            const bool loaded = perform_request(url, response) &&
                                !is_http_error(response);
            if (loaded) {
                pending_stylesheets_.push_back(widen(
                    std::string(response.body.begin(), response.body.end())));
                layout_dirty_ = true;
            } else {
                record_diagnostic(LoadDiagnosticKind::Resource, url,
                                  "failed to fetch dynamic stylesheet");
            }
            binding_->dispatch_event(node, loaded ? u"load" : u"error", false, false);
        });
        return;
    }

    const std::string type = trim_ascii_lower(attribute(u"type"));
    const std::u16string src = attribute(u"src");
    const bool external = !src.empty();
    const std::string url = external
        ? resolve_url(src)
        : current_url_ + "#dynamic-script-" + std::to_string(key);
    const bool module = type == "module";
    if (!module && tree_->get_attribute(node, u"nomodule").has_value())
        return;
    if (!module && !is_classic_script_type(type)) return;

    std::string inline_source = external ? std::string() :
        narrow(tree_->text_content(node));
    dom::Document* document = doc_.get();
    engine_.event_loop().post_task(
        [this, document, node, external, url, module,
         inline_source = std::move(inline_source)]() {
            if (doc_.get() != document) return;
            const bool trace_scripts = std::getenv("MALIBU_TRACE_SCRIPTS") != nullptr;
            std::string source = inline_source;
            bool loaded = true;
            if (external) {
                loaded = load_script_source(url, source);
                if (!loaded)
                    record_diagnostic(LoadDiagnosticKind::Resource, url,
                                      "failed to fetch dynamic script");
            }

            js::Engine::EvalResult result;
            if (loaded) {
                if (trace_scripts)
                    std::fprintf(stderr, "[script] start %s (dynamic)\n", url.c_str());
                set_import_meta(engine_, url);
                result = module ? engine_.evaluate_module(source, url)
                                : engine_.evaluate(source, url);
                if (trace_scripts)
                    std::fprintf(stderr, "[script] end %s (%s, dynamic)\n",
                                 url.c_str(), result.ok ? "ok" : "error");
                if (!result.ok)
                    record_diagnostic(LoadDiagnosticKind::Script, url, result.error);
            }
            binding_->dispatch_event(
                node, loaded && result.ok ? u"load" : u"error", false, false);
        });
}

void View::record_diagnostic(LoadDiagnosticKind kind, std::string url,
                             std::string message) {
    constexpr size_t kMaxMessageLength = 4096;
    if (message.size() > kMaxMessageLength) {
        message.resize(kMaxMessageLength);
        message += "...";
    }
    diagnostics_.push_back({kind, std::move(url), std::move(message)});
    if (diagnostic_handler_) diagnostic_handler_(diagnostics_.back());
}

void View::fire_window_event(const std::u16string& type) {
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArray;
    auto& interp = engine_.interpreter();
    Value* we = interp.global()->find(u"__winEvents");
    if (!we) return;
    Value arrv = interp.get_prop_public(*we, type);
    if (!arrv.is_heap_ptr() || arrv.as_heap_ptr()->kind != js::vm::HeapObject::kJSArray) return;
    JSObject* ev = interp.new_object();
    ev->set(u"type", interp.str(narrow(type)));
    ev->set(u"target", *we);
    // Snapshot listeners so mutation during dispatch is safe.
    std::vector<Value> cbs = static_cast<JSArray*>(arrv.as_heap_ptr())->elements;
    for (Value cb : cbs) {
        if (!interp.is_callable(cb)) continue;
        std::vector<Value> args{ Value::make_heap_ptr(ev) };
        try { interp.call(cb, Value::make_undefined(), args); }
        catch (js::runtime::ThrowSignal&) {}
    }
    interp.run_microtasks();
}

void View::do_load(const std::string& html, const std::string& base_url) {
    layout_dirty_ = true;  // a fresh document always needs styling + layout
    diagnostics_.clear();
    // Set the URL/origin BEFORE reset_document so install_view_globals builds
    // `location` from the page being loaded (not the previous one).
    current_url_ = base_url;
    origin_ = security::Origin::parse(base_url);
    reset_document();

    html::HTMLParser parser;
    auto parsed = parser.parse(widen(html), *tree_);

    // External stylesheets (<link rel=stylesheet>) fetched via the host, then
    // inline <style> blocks — author order so later rules win.
    pending_stylesheets_.clear();
    for (const std::u16string& href : parsed.external_styles) {
        const std::string url = resolve_url(href);
        if (!fetch_handler_ && !request_handler_) {
            record_diagnostic(LoadDiagnosticKind::Resource, url,
                              "no resource loader is installed for stylesheet");
            continue;
        }
        network::FetchResponse resp;
        if (!perform_request(url, resp)) {
            record_diagnostic(LoadDiagnosticKind::Resource, url,
                              "failed to fetch stylesheet");
            continue;
        }
        if (is_http_error(resp)) {
            record_diagnostic(
                LoadDiagnosticKind::Resource, url,
                "stylesheet request returned HTTP " +
                    std::to_string(resp.status));
            continue;
        }
        pending_stylesheets_.push_back(
            widen(std::string(resp.body.begin(), resp.body.end())));
    }
    for (auto& s : parsed.stylesheets) pending_stylesheets_.push_back(s);

    apply_styles();                 // cascade is needed to rasterize inline SVG
    load_images(parsed);            // decode replaced elements + intrinsic sizes
    apply_styles();                 // form-control defaults currently mutate style attrs
    run_scripts(parsed.script_items);  // inline + external scripts, in document order
    binding_->dispatch_event(doc_->root(), u"DOMContentLoaded", true, false);
    fire_window_event(u"DOMContentLoaded");
    fire_window_event(u"load");
    run_tasks();                    // settle page and worker load handlers
    apply_styles();                 // re-style after script mutations
}

bool View::load_html(const std::string& html, const std::string& base_url) {
    do_load(html, base_url);
    // Push a new history entry, dropping any forward entries.
    history_.resize(history_.empty() ? 0 : history_pos_ + 1);
    history_html_.resize(history_.size());
    history_.push_back(base_url);
    history_html_.push_back(html);
    history_pos_ = history_.size() - 1;
    process_pending_navigation();
    return true;
}

bool View::load_file(const std::string& path) {
    if (sandbox_ & SandboxNoNavigation) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return load_html(ss.str(), "file://" + path);
}

bool View::load_url(const std::string& url) {
    if (sandbox_ & SandboxNoNavigation) return false;
    // Request interception lets the host satisfy the navigation locally.
    network::FetchResponse resp;
    if (perform_request(url, resp)) {
        return load_html(
            std::string(resp.body.begin(), resp.body.end()),
            resp.url.empty() ? url : resp.url);
    }
    if (sandbox_ & SandboxNoNetwork) return false;
    // No transport is wired into the view by default; embedders intercept via
    // set_request_handler. (The FetchEngine is exercised separately.)
    return false;
}

void View::reload() {
    if (history_pos_ < history_html_.size())
        do_load(history_html_[history_pos_], history_[history_pos_]);
}

bool View::go_back() {
    if (history_pos_ == 0 || history_.empty()) return false;
    --history_pos_;
    do_load(history_html_[history_pos_], history_[history_pos_]);
    return true;
}

bool View::go_forward() {
    if (history_pos_ + 1 >= history_.size()) return false;
    ++history_pos_;
    do_load(history_html_[history_pos_], history_[history_pos_]);
    return true;
}

bool View::dispatch_event(const std::string& selector, const std::string& type, bool bubbles) {
    if (!binding_) return false;
    malibu::NodeHandle node = tree_->query_selector(doc_->root(), widen(selector));
    if (node.is_null()) return false;
    layout_dirty_ = true;  // a JS handler may mutate the DOM
    return binding_->dispatch_event(node, widen(type), bubbles, /*cancelable=*/true);
}

std::string View::eval_js(const std::string& source) {
    layout_dirty_ = true;  // evaluated JS may mutate the DOM/styles
    auto r = engine_.evaluate(source, current_url_);
    if (!r.ok) return "error: " + r.error;
    std::string result =
        narrow(engine_.interpreter().json_stringify(r.value));
    process_pending_navigation();
    return result;
}

void View::run_tasks(uint64_t elapsed_ms) {
    engine_.run_ready_tasks(elapsed_ms);
    const auto workers = workers_;
    for (const auto& worker : workers) {
        if (!worker->terminated)
            worker->engine.run_ready_tasks(elapsed_ms);
    }
    // Worker postMessage() queues tasks back on the owning page.
    engine_.run_ready_tasks();
    process_pending_navigation();
}

render::Framebuffer View::render(int width, int height, float scroll_y) {
    // Restyle+relayout only when content or viewport changed. A pure scroll keeps
    // the cached layout tree and just re-rasterizes at the new offset — this is the
    // difference between re-parsing all CSS + relaying-out every wheel tick (very
    // slow) and an instant scroll.
    bool relayout = layout_dirty_ || width != last_vw_ || height != last_vh_ || layout_.root() == nullptr;
    last_vw_ = width; last_vh_ = height;
    layout::LayoutBox* root;
    if (relayout) {
        apply_styles();
        if (replaced_content_dirty_)
            load_images(malibu::html::ParsedDocument{}, false, false);
        root = layout_.layout_document(*doc_, static_cast<float>(width), static_cast<float>(height));
        layout_dirty_ = false;
    } else {
        root = layout_.root();
    }
    malibu::render::Color canvas_bg = {255, 255, 255, 255};
    if (root && root->children.size() == 1) {
        malibu::layout::LayoutBox* html_box = root->children[0];
        if (html_box && html_box->style && html_box->node.index) {
            auto* html_core = doc_->core(html_box->node);
            if (html_core && html_core->tag_name == u"html") {
                if (!html_box->style->background_color.transparent()) {
                    canvas_bg = html_box->style->background_color;
                } else {
                    for (malibu::layout::LayoutBox* child : html_box->children) {
                        if (child && child->style && child->node.index) {
                            auto* body_core = doc_->core(child->node);
                            if (body_core && body_core->tag_name == u"body" && !child->style->background_color.transparent()) {
                                canvas_bg = child->style->background_color;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    render::Framebuffer fb = renderer_.render(*doc_, root, width, height, canvas_bg,
                                              drawer_.get(), scroll_y);
    composite_canvases(fb, scroll_y);  // blit <canvas>/<img> bitmaps over the page
    return fb;
}

malibu::NodeHandle View::node_at(float x, float y) {
    layout::LayoutBox* b = layout_.hit_test(x, y);
    return b ? b->node : malibu::NodeHandle::null_handle();
}

bool View::set_hover(float x, float y) {
    layout::LayoutBox* b = layout_.hit_test(x, y);
    malibu::NodeHandle hit = b ? b->node : malibu::NodeHandle::null_handle();
    if (hit == hovered_) return false;
    // Clear the old hover chain, set the new one (ancestors get :hover too).
    for (malibu::NodeHandle n = hovered_; doc_->core(n); n = doc_->core(n)->parent) doc_->core(n)->hovered = false;
    hovered_ = hit;
    for (malibu::NodeHandle n = hovered_; doc_->core(n); n = doc_->core(n)->parent) doc_->core(n)->hovered = true;
    layout_dirty_ = true;  // :hover changed → restyle
    return true;
}

malibu::NodeHandle View::dispatch_mouse(float x, float y, const std::string& type, int button) {
    (void)button;
    malibu::NodeHandle hit = node_at(x, y);
    if (!doc_->core(hit)) return hit;
    layout_dirty_ = true;  // :active/:focus change + JS handlers may mutate the DOM
    if (type == "mousedown") doc_->core(hit)->active = true;
    if (type == "click") {                       // move focus to the clicked element
        if (doc_->core(focused_)) doc_->core(focused_)->focused = false;
        focused_ = hit;
        doc_->core(focused_)->focused = true;
    }
    binding_->dispatch_event(hit, widen(type), /*bubbles=*/true, /*cancelable=*/true);
    run_tasks();
    if ((type == "mouseup" || type == "click") && doc_->core(hit)) doc_->core(hit)->active = false;
    return hit;
}

void View::dispatch_key(const std::string& key, bool is_text) {
    auto* c = doc_->core(focused_);
    if (!c || (c->tag_name != u"input" && c->tag_name != u"textarea")) return;
    layout_dirty_ = true;  // editing the value + input/change handlers
    std::u16string val = tree_->get_attribute(focused_, u"value").value_or(u"");
    if (is_text) val += widen(key);
    else if (key == "Backspace") { if (!val.empty()) val.pop_back(); }
    else if (key == "Enter") { binding_->dispatch_event(focused_, u"change", true, true); run_tasks(); return; }
    else return;
    tree_->set_attribute(focused_, u"value", val);
    binding_->dispatch_event(focused_, u"input", true, true);
    run_tasks();
}

float View::page_height(int width) {
    last_vw_ = width;
    apply_styles();
    layout::LayoutBox* root = layout_.layout_document(*doc_, static_cast<float>(width), 800.0f);
    return root ? root->height : 0.0f;
}

void View::post_message(const std::string& message) {
    auto& interp = engine_.interpreter();
    js::runtime::Value* gt = interp.global()->find(u"globalThis");
    if (!gt) return;
    js::runtime::Value handler = interp.get_prop_public(*gt, u"__malibuReceiveMessage");
    if (interp.is_callable(handler)) {
        std::vector<js::runtime::Value> args{interp.str(message)};
        try { interp.call(handler, js::runtime::Value::make_undefined(), args); }
        catch (js::runtime::ThrowSignal&) {}
        interp.run_microtasks();
    }
}

void View::dispatch_socket_event(int id, const char* type,
                                 const std::string& data, int code,
                                 const std::string& reason) {
    auto found = std::find_if(
        sockets_.begin(), sockets_.end(),
        [id](const auto& entry) { return entry.first == id; });
    if (found == sockets_.end()) return;

    auto& interp = engine_.interpreter();
    js::runtime::Value socket = found->second;
    auto* event = interp.new_object();
    event->set(u"type", interp.str(type));
    event->set(u"target", socket);
    event->set(u"currentTarget", socket);
    event->set(u"data", interp.str(data));
    event->set(u"code", js::runtime::Value::make_int32(code));
    event->set(u"reason", interp.str(reason));
    event->set(
        u"wasClean",
        js::runtime::Value::make_bool(code == 1000 || code == 1001));
    js::runtime::Value event_value =
        js::runtime::Value::make_heap_ptr(event);
    std::vector<js::runtime::Value> arguments{event_value};

    auto invoke = [&](js::runtime::Value callback) {
        if (!interp.is_callable(callback)) return;
        try {
            interp.call(callback, socket, arguments);
        } catch (js::runtime::ThrowSignal&) {
        }
    };
    invoke(interp.get_prop_public(
        socket, widen(std::string("on") + type)));

    js::runtime::Value listeners_value =
        interp.get_prop_public(socket, u"%listeners%");
    if (listeners_value.is_heap_ptr() &&
        listeners_value.as_heap_ptr()->kind ==
            js::vm::HeapObject::kJSObject) {
        auto* listeners = static_cast<js::runtime::JSObject*>(
            listeners_value.as_heap_ptr());
        js::runtime::Value callbacks_value =
            listeners->get(widen(type));
        if (callbacks_value.is_heap_ptr() &&
            callbacks_value.as_heap_ptr()->kind ==
                js::vm::HeapObject::kJSArray) {
            auto* callbacks = static_cast<js::runtime::JSArray*>(
                callbacks_value.as_heap_ptr());
            std::vector<js::runtime::Value> snapshot =
                callbacks->elements;
            for (size_t i = 0; i < snapshot.size(); ++i)
                if (callbacks->has_index(i)) invoke(snapshot[i]);
        }
    }
    interp.run_microtasks();
}

void View::socket_open(int id) {
    auto found = std::find_if(
        sockets_.begin(), sockets_.end(),
        [id](const auto& entry) { return entry.first == id; });
    if (found == sockets_.end()) return;
    auto& interp = engine_.interpreter();
    if (interp.to_int32(
            interp.get_prop_public(found->second, u"readyState")) != 0)
        return;
    interp.set_prop_public(
        found->second, u"readyState",
        js::runtime::Value::make_int32(1));
    dispatch_socket_event(id, "open", "", 0, "");
}

void View::socket_message(int id, const std::string& data) {
    auto found = std::find_if(
        sockets_.begin(), sockets_.end(),
        [id](const auto& entry) { return entry.first == id; });
    if (found == sockets_.end()) return;
    auto& interp = engine_.interpreter();
    if (interp.to_int32(
            interp.get_prop_public(found->second, u"readyState")) != 1)
        return;
    dispatch_socket_event(id, "message", data, 0, "");
}

void View::socket_close(int id, int code, const std::string& reason) {
    auto found = std::find_if(
        sockets_.begin(), sockets_.end(),
        [id](const auto& entry) { return entry.first == id; });
    if (found == sockets_.end()) return;
    auto& interp = engine_.interpreter();
    interp.set_prop_public(
        found->second, u"readyState",
        js::runtime::Value::make_int32(3));
    if (code == 1006)
        dispatch_socket_event(id, "error", "", code, reason);
    dispatch_socket_event(id, "close", "", code, reason);
    interp.remove_host_root(found->second);
    sockets_.erase(found);
}

} // namespace malibu::view
