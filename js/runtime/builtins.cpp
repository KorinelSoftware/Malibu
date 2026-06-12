// js/runtime/builtins.cpp
// Installs the MalibuJS standard library: console, Math, Object, Array, JSON,
// String/Number/Boolean, global functions, and Array/String/Object prototypes.

#include "malibu/js/runtime/interpreter.h"
#include "malibu/js/runtime/regex.h"
#include "malibu/event_loop/event_loop.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

namespace malibu::js::runtime {
namespace {
std::u16string u16(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string narrow(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }
Value arg(std::vector<Value>& a, size_t i) { return i < a.size() ? a[i] : Value::make_undefined(); }

constexpr double kMaxDateMilliseconds = 8.64e15;

double time_clip(double milliseconds) {
    if (!std::isfinite(milliseconds) ||
        std::abs(milliseconds) > kMaxDateMilliseconds)
        return std::numeric_limits<double>::quiet_NaN();
    return std::trunc(milliseconds);
}

bool split_date(double milliseconds, std::tm& output, int& subsecond) {
    if (!std::isfinite(milliseconds)) return false;
    double whole_seconds = std::floor(milliseconds / 1000.0);
    double remainder = milliseconds - whole_seconds * 1000.0;
    std::time_t seconds = static_cast<std::time_t>(whole_seconds);
    if (!gmtime_r(&seconds, &output)) return false;
    subsecond = static_cast<int>(remainder);
    return true;
}

double join_date(std::tm parts, int subsecond) {
    std::time_t seconds = timegm(&parts);
    return time_clip(static_cast<double>(seconds) * 1000.0 + subsecond);
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) {
    static constexpr int days[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 1) return leap_year(year) ? 29 : 28;
    return month >= 0 && month < 12 ? days[month] : 0;
}

double parse_date_string(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    if (text.empty()) return std::numeric_limits<double>::quiet_NaN();

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    double second = 0;
    int consumed = 0;
    int fields = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%lf%n",
                             &year, &month, &day, &hour, &minute, &second,
                             &consumed);
    if (fields != 6) {
        consumed = 0;
        fields = std::sscanf(text.c_str(), "%d-%d-%d%n",
                             &year, &month, &day, &consumed);
    }
    if (fields >= 3) {
        if (month < 1 || month > 12 || day < 1 ||
            day > days_in_month(year, month - 1) || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 || second < 0 || second >= 60)
            return std::numeric_limits<double>::quiet_NaN();
        std::tm parts{};
        parts.tm_year = year - 1900;
        parts.tm_mon = month - 1;
        parts.tm_mday = day;
        parts.tm_hour = hour;
        parts.tm_min = minute;
        parts.tm_sec = static_cast<int>(second);
        double result =
            static_cast<double>(timegm(&parts)) * 1000.0 +
            std::trunc((second - std::floor(second)) * 1000.0);
        std::string suffix = text.substr(static_cast<size_t>(consumed));
        if (!suffix.empty() && suffix != "Z" && suffix != "z") {
            int offset_hour = 0;
            int offset_minute = 0;
            char sign = '\0';
            if (std::sscanf(suffix.c_str(), "%c%d:%d", &sign, &offset_hour,
                            &offset_minute) < 2 ||
                (sign != '+' && sign != '-') || offset_hour > 23 ||
                offset_minute > 59)
                return std::numeric_limits<double>::quiet_NaN();
            double offset =
                static_cast<double>(offset_hour * 60 + offset_minute) *
                60.0 * 1000.0;
            result += sign == '+' ? -offset : offset;
        }
        return time_clip(result);
    }

    std::tm parts{};
    char* end = strptime(text.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &parts);
    if (!end)
        end = strptime(text.c_str(), "%a %b %d %Y %H:%M:%S GMT%z", &parts);
    if (end && *end == '\0')
        return time_clip(static_cast<double>(timegm(&parts)) * 1000.0);
    return std::numeric_limits<double>::quiet_NaN();
}

std::string encode_utf8(const std::u16string& input) {
    std::string output;
    for (size_t i = 0; i < input.size(); ++i) {
        char32_t codepoint = input[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
            i + 1 < input.size() && input[i + 1] >= 0xDC00 &&
            input[i + 1] <= 0xDFFF) {
            codepoint =
                0x10000 + ((codepoint - 0xD800) << 10) +
                (input[++i] - 0xDC00);
        }
        if (codepoint < 0x80) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            output.push_back(
                static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(
                static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            output.push_back(
                static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(
                0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(
                static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(
                0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(
                0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return output;
}

std::u16string decode_utf8(const std::vector<uint8_t>& bytes) {
    std::u16string output;
    size_t i = 0;
    while (i < bytes.size()) {
        uint8_t first = bytes[i];
        char32_t codepoint = 0;
        size_t extra = 0;
        if (first < 0x80) {
            codepoint = first;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            extra = 1;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            extra = 2;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            extra = 3;
        } else {
            output.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= bytes.size()) {
            output.push_back(0xFFFD);
            break;
        }
        bool valid = true;
        for (size_t j = 1; j <= extra; ++j) {
            uint8_t continuation = bytes[i + j];
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint =
                (codepoint << 6) | (continuation & 0x3F);
        }
        if (!valid || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            output.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += extra + 1;
        if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char16_t>(codepoint));
        } else {
            codepoint -= 0x10000;
            output.push_back(static_cast<char16_t>(
                0xD800 + (codepoint >> 10)));
            output.push_back(static_cast<char16_t>(
                0xDC00 + (codepoint & 0x3FF)));
        }
    }
    return output;
}

void trace_host_callback(const char* phase, const char* kind, Value callback,
                         uint64_t delay = 0) {
    if (!std::getenv("MALIBU_TRACE_TASKS")) return;
    const char* source = "<non-function>";
    std::string name;
    uint32_t line = 0;
    if (callback.is_heap_ptr() &&
        callback.as_heap_ptr()->kind == HeapObject::kJSFunction) {
        auto* function = static_cast<JSFunction*>(callback.as_heap_ptr());
        name = narrow(function->name);
        if (function->code) {
            source = function->code->source_name.c_str();
            line = function->code->source_line;
        } else {
            source = "<native>";
        }
    }
    std::fprintf(stderr, "[task] %s %s delay=%llu %s %s:%u\n",
                 phase, kind, static_cast<unsigned long long>(delay),
                 name.c_str(), source, line);
}

// RAII GC root for a heap temporary that a native holds across a call back into
// JS (which may trigger a collection). `in.call(...)` unwinds via a C++
// exception on a JS throw, so manual push/pop_root would unbalance the root
// stack — scope-bound rooting keeps it correct on both return and throw.
struct Root {
    Interpreter& in;
    explicit Root(Interpreter& i, Value v) : in(i) { in.push_root(v); }
    ~Root() { in.pop_root(); }
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
};

// Returns the value as a JSObject* iff it carries a prototype slot (i.e. its
// runtime type derives from JSObject); otherwise nullptr. JSString / JSFunction
// / DomNodeRef / Environment derive directly from HeapObject and have NO proto
// member, so `static_cast<JSObject*>(...)->proto` on them reads past their
// layout and crashes — every `->proto` site must funnel through this guard.
JSObject* as_proto_object(Value v) {
    if (!v.is_heap_ptr()) return nullptr;
    switch (v.as_heap_ptr()->kind) {
        case HeapObject::kJSObject:  case HeapObject::kJSArray:
        case HeapObject::kJSPromise: case HeapObject::kJSMap:
        case HeapObject::kJSSet:     case HeapObject::kJSGenerator:
        case HeapObject::kArrayBuffer: case HeapObject::kTypedArray:
        case HeapObject::kDataView:  case HeapObject::kJSProxy:
        case HeapObject::kJSFunction:
            return static_cast<JSObject*>(v.as_heap_ptr());
        default:  // kJSString, kDomNodeRef, kEnvironment
            return nullptr;
    }
}

// Parses a canonical array index ("0", "12", ...) — no leading zeros, in range.
bool parse_index_u16(const std::u16string& s, size_t& out) {
    if (s.empty() || s.size() > 10) return false;
    if (s.size() > 1 && s[0] == u'0') return false;
    size_t v = 0;
    for (char16_t c : s) { if (c < u'0' || c > u'9') return false; v = v * 10 + (c - u'0'); }
    out = v; return true;
}

// A regex object is a plain JSObject carrying a hidden %isRegExp% flag plus
// `source`/`flags`. Returns false (leaving outputs untouched) for non-regexes.
bool get_regex(Interpreter& in, Value v, std::u16string& src, bool& g, bool& ic, bool& ml) {
    if (!v.is_heap_ptr() || v.as_heap_ptr()->kind != HeapObject::kJSObject) return false;
    JSObject* o = static_cast<JSObject*>(v.as_heap_ptr());
    if (!o->has_own(u"%isRegExp%")) return false;
    src = in.to_string(o->get(u"source"));
    std::u16string fl = in.to_string(o->get(u"flags"));
    g  = fl.find('g') != std::u16string::npos;
    ic = fl.find('i') != std::u16string::npos;
    ml = fl.find('m') != std::u16string::npos;
    return true;
}

// Expands a String.replace template ($&, $1..$99, $$) against a match.
std::u16string expand_replacement(const std::u16string& tmpl, const std::u16string& s,
                                  const regex::Match& mr) {
    std::u16string out;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '$' && i + 1 < tmpl.size()) {
            char16_t n = tmpl[i + 1];
            if (n == '$') { out += '$'; ++i; continue; }
            if (n == '&') { out += s.substr(mr.index, mr.end - mr.index); ++i; continue; }
            if (n >= '0' && n <= '9') {
                size_t gi = n - '0'; size_t adv = 1;
                if (i + 2 < tmpl.size() && tmpl[i + 2] >= '0' && tmpl[i + 2] <= '9') {
                    size_t two = gi * 10 + (tmpl[i + 2] - '0');
                    if (two < mr.groups.size()) { gi = two; adv = 2; }
                }
                if (gi >= 1 && gi < mr.groups.size()) {
                    auto [lo, hi] = mr.groups[gi];
                    if (lo >= 0) out += s.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo));
                    i += adv; continue;
                }
            }
        }
        out += tmpl[i];
    }
    return out;
}

// Recursive-descent JSON parser (ECMA-404) producing engine Values.
struct JsonParser {
    Interpreter&          in;
    const std::u16string& s;
    size_t                i = 0;
    bool                  ok = true;

    void ws() { while (i < s.size() && (s[i] == u' ' || s[i] == u'\t' || s[i] == u'\n' || s[i] == u'\r')) ++i; }

    Value value() {
        ws();
        if (i >= s.size()) { ok = false; return Value::make_undefined(); }
        switch (s[i]) {
            case u'{': return object();
            case u'[': return array();
            case u'"': return in.str(string());
            case u't': if (i + 4 <= s.size() && s.compare(i, 4, u"true") == 0)  { i += 4; return Value::make_bool(true); }  ok = false; return Value::make_undefined();
            case u'f': if (i + 5 <= s.size() && s.compare(i, 5, u"false") == 0) { i += 5; return Value::make_bool(false); } ok = false; return Value::make_undefined();
            case u'n': if (i + 4 <= s.size() && s.compare(i, 4, u"null") == 0)  { i += 4; return Value::make_null(); }      ok = false; return Value::make_undefined();
            default:   return number();
        }
    }

    Value object() {
        JSObject* o = in.new_object();
        Value ov = Value::make_heap_ptr(o);
        ++i; ws();
        if (i < s.size() && s[i] == u'}') { ++i; return ov; }
        while (i < s.size() && ok) {
            ws();
            if (i >= s.size() || s[i] != u'"') { ok = false; break; }
            std::u16string key = string();
            ws();
            if (i >= s.size() || s[i] != u':') { ok = false; break; }
            ++i;
            o->set(key, value());
            ws();
            if (i < s.size() && s[i] == u',') { ++i; continue; }
            if (i < s.size() && s[i] == u'}') { ++i; break; }
            ok = false;
        }
        return ov;
    }

    Value array() {
        JSArray* a = in.new_array();
        Value av = Value::make_heap_ptr(a);
        ++i; ws();
        if (i < s.size() && s[i] == u']') { ++i; return av; }
        while (i < s.size() && ok) {
            a->elements.push_back(value());
            ws();
            if (i < s.size() && s[i] == u',') { ++i; continue; }
            if (i < s.size() && s[i] == u']') { ++i; break; }
            ok = false;
        }
        return av;
    }

    std::u16string string() {
        std::u16string r;
        ++i;  // opening quote
        while (i < s.size()) {
            char16_t c = s[i++];
            if (c == u'"') return r;
            if (c == u'\\' && i < s.size()) {
                char16_t e = s[i++];
                switch (e) {
                    case u'"': r += u'"'; break;  case u'\\': r += u'\\'; break; case u'/': r += u'/'; break;
                    case u'n': r += u'\n'; break; case u't': r += u'\t'; break; case u'r': r += u'\r'; break;
                    case u'b': r += u'\b'; break; case u'f': r += u'\f'; break;
                    case u'u': {
                        int cp = 0;
                        if (i + 4 <= s.size()) {
                            for (int k = 0; k < 4; ++k) {
                                char16_t h = s[i++]; cp <<= 4;
                                if (h >= u'0' && h <= u'9') cp |= h - u'0';
                                else if (h >= u'a' && h <= u'f') cp |= h - u'a' + 10;
                                else if (h >= u'A' && h <= u'F') cp |= h - u'A' + 10;
                            }
                        }
                        r += static_cast<char16_t>(cp);
                        break;
                    }
                    default: r += e; break;
                }
            } else {
                r += c;
            }
        }
        ok = false;
        return r;
    }

    Value number() {
        size_t start = i;
        if (i < s.size() && (s[i] == u'-' || s[i] == u'+')) ++i;
        while (i < s.size() && ((s[i] >= u'0' && s[i] <= u'9') || s[i] == u'.' ||
                                s[i] == u'e' || s[i] == u'E' || s[i] == u'+' || s[i] == u'-')) ++i;
        std::string num;
        for (size_t k = start; k < i; ++k) num.push_back(static_cast<char>(s[k] & 0xFF));
        try { return Value::make_double(std::stod(num)); }
        catch (...) { ok = false; return Value::make_undefined(); }
    }
};

JSArray* as_array(Interpreter& in, Value v) {
    if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSArray)
        return static_cast<JSArray*>(v.as_heap_ptr());
    in.throw_error(u"TypeError", u"not an array");
}

// Generic element snapshot for the (non-mutating) Array.prototype methods so
// they work on array-likes too (`Array.prototype.slice.call(arguments)`,
// `[].map.call(nodeList, ...)`). The source stays rooted via `t`, so the
// snapshotted Values remain reachable across callbacks.
std::vector<Value> elements_of(Interpreter& in, Value t) {
    if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSArray)
        return static_cast<JSArray*>(t.as_heap_ptr())->elements;
    if (t.is_undefined() || t.is_null())
        in.throw_error(u"TypeError", u"Array.prototype method called on null or undefined");
    std::vector<Value> out;
    double len = in.to_number(in.get_prop_public(t, u"length"));
    if (std::isnan(len) || len <= 0) return out;
    size_t n = len > 4294967295.0 ? 4294967295u : static_cast<size_t>(len);
    out.reserve(n < 1024 ? n : 1024);
    for (size_t i = 0; i < n; ++i)
        out.push_back(in.get_prop_public(t, u16(std::to_string(i))));
    return out;
}

bool element_present(Value value, size_t index) {
    return !value.is_heap_ptr() ||
           value.as_heap_ptr()->kind != HeapObject::kJSArray ||
           static_cast<JSArray*>(value.as_heap_ptr())->has_index(index);
}

bool has_own_property(Value value, const std::u16string& key) {
    if (!value.is_heap_ptr()) return false;
    HeapObject* object = value.as_heap_ptr();
    if (object->kind == HeapObject::kJSArray) {
        auto* array = static_cast<JSArray*>(object);
        size_t index;
        if (key == u"length") return true;
        if (parse_index_u16(key, index)) return array->has_index(index);
        return array->has_own(key);
    }
    if (object->kind == HeapObject::kJSObject ||
        object->kind == HeapObject::kJSMap ||
        object->kind == HeapObject::kJSSet ||
        object->kind == HeapObject::kJSPromise ||
        object->kind == HeapObject::kJSGenerator)
        return static_cast<JSObject*>(object)->has_own(key);
    if (object->kind == HeapObject::kJSFunction) {
        auto* function = static_cast<JSFunction*>(object);
        return key == u"name" || key == u"length" ||
               function->find_own(key) != nullptr;
    }
    if (object->kind == HeapObject::kJSString) {
        auto* string = static_cast<JSString*>(object);
        size_t index;
        return key == u"length" ||
               (parse_index_u16(key, index) && index < string->data.size());
    }
    return false;
}

Value array_iterator(Interpreter& in, JSArray* values) {
    Value array = Value::make_heap_ptr(values);
    Value iterator_method = in.get_prop_public(array, u"@@iterator");
    std::vector<Value> args;
    return in.call(iterator_method, array, args);
}

Value wrapped_primitive(Value v) {
    if (!v.is_heap_ptr() || v.as_heap_ptr()->kind != HeapObject::kJSObject)
        return Value::make_undefined();
    JSObject* object = static_cast<JSObject*>(v.as_heap_ptr());
    return object->has_own(u"%primitive%") ? object->get(u"%primitive%")
                                           : Value::make_undefined();
}
std::u16string as_str(Interpreter& in, Value v) {
    Value primitive = wrapped_primitive(v);
    if (primitive.is_heap_ptr() && primitive.as_heap_ptr()->kind == HeapObject::kJSString)
        return static_cast<JSString*>(primitive.as_heap_ptr())->data;
    return in.to_string(v);
}
double number_this(Interpreter& in, Value value) {
    if (value.is_int32()) return value.as_int32();
    if (value.is_double()) return value.as_double();
    Value primitive = wrapped_primitive(value);
    if (primitive.is_int32()) return primitive.as_int32();
    if (primitive.is_double()) return primitive.as_double();
    in.throw_error(u"TypeError", u"Number method called on incompatible receiver");
}
bool boolean_this(Interpreter& in, Value value) {
    if (value.is_bool()) return value.as_bool();
    Value primitive = wrapped_primitive(value);
    if (primitive.is_bool()) return primitive.as_bool();
    in.throw_error(u"TypeError", u"Boolean method called on incompatible receiver");
}
Value bigint_this(Interpreter& in, Value value) {
    if (value.is_heap_ptr() && value.as_heap_ptr()->kind == HeapObject::kJSBigInt)
        return value;
    Value primitive = wrapped_primitive(value);
    if (primitive.is_heap_ptr() && primitive.as_heap_ptr()->kind == HeapObject::kJSBigInt)
        return primitive;
    in.throw_error(u"TypeError", u"BigInt method called on incompatible receiver");
}
unsigned long to_index(Interpreter& in, Value value) {
    double number = in.to_number(value);
    if (std::isnan(number) || number == 0) return 0;
    double integer = std::trunc(number);
    if (!std::isfinite(integer) || integer < 0 || integer > 9007199254740991.0 ||
        integer > static_cast<double>(std::numeric_limits<unsigned long>::max()))
        in.throw_error(u"RangeError", u"Invalid index");
    return static_cast<unsigned long>(integer);
}

double to_integer_or_infinity(Interpreter& in, Value value) {
    const double number = in.to_number(value);
    if (std::isnan(number) || number == 0.0) return 0.0;
    if (!std::isfinite(number)) return number;
    return std::trunc(number);
}

size_t relative_string_index(double index, size_t length) {
    const double size = static_cast<double>(length);
    if (index == -std::numeric_limits<double>::infinity()) return 0;
    if (index < 0.0)
        return static_cast<size_t>(std::max(size + index, 0.0));
    if (index >= size) return length;
    return static_cast<size_t>(index);
}

size_t clamped_string_index(double index, size_t length) {
    if (index <= 0.0) return 0;
    if (index >= static_cast<double>(length)) return length;
    return static_cast<size_t>(index);
}
}  // namespace

void Interpreter::install_builtins() {
    auto def = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
        global_->define(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)));
    };
    auto method = [&](JSObject* proto, const char* name, NativeFn fn, uint32_t arity = 0) {
        proto->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
    };

    // ---- value constants ----
    global_->define(u"undefined", Value::make_undefined());
    global_->define(u"NaN", Value::make_double(std::nan("")));
    global_->define(u"Infinity", Value::make_double(std::numeric_limits<double>::infinity()));

    // globalThis is the object backing the global scope: global var/function
    // declarations and bare global lookups are its own properties.
    global_->define(u"globalThis", Value::make_heap_ptr(global_object_));

    // ---- console ----
    {
        JSObject* console = new_object();
        auto write_console = [](Interpreter& in, const std::vector<Value>& args) {
            std::u16string line;
            for (size_t i = 0; i < args.size(); ++i) { if (i) line += u" "; line += in.to_string(args[i]); }
            std::string s = narrow(line);
            if (std::getenv("MALIBU_TRACE_CONSOLE"))
                in.trace_call_stack_for_diagnostics("console output");
            if (in.console_sink()) in.console_sink()->log(s);
            else std::cout << s << "\n";
        };
        auto logger = [write_console](Interpreter& in, Value, std::vector<Value>& args) -> Value {
            write_console(in, args);
            return Value::make_undefined();
        };
        console->set(u"log", Value::make_heap_ptr(new_native(u"log", logger)));
        console->set(u"debug", Value::make_heap_ptr(new_native(u"debug", logger)));
        console->set(u"info", Value::make_heap_ptr(new_native(u"info", logger)));
        console->set(u"warn", Value::make_heap_ptr(new_native(u"warn", logger)));
        console->set(u"error", Value::make_heap_ptr(new_native(u"error", logger)));
        console->set(u"trace", Value::make_heap_ptr(new_native(u"trace", logger)));
        console->set(u"dir", Value::make_heap_ptr(new_native(u"dir", logger)));
        console->set(u"table", Value::make_heap_ptr(new_native(u"table", logger)));
        console->set(u"assert", Value::make_heap_ptr(new_native(
            u"assert",
            [write_console](Interpreter& in, Value, std::vector<Value>& args) -> Value {
                if (args.empty() || !in.to_bool(args[0])) {
                    std::vector<Value> message;
                    if (args.size() > 1)
                        message.assign(args.begin() + 1, args.end());
                    else
                        message.push_back(in.str("Assertion failed"));
                    write_console(in, message);
                }
                return Value::make_undefined();
            },
            1)));
        auto noop = [](Interpreter&, Value, std::vector<Value>&) -> Value {
            return Value::make_undefined();
        };
        console->set(u"group", Value::make_heap_ptr(new_native(u"group", noop)));
        console->set(u"groupCollapsed", Value::make_heap_ptr(new_native(u"groupCollapsed", noop)));
        console->set(u"groupEnd", Value::make_heap_ptr(new_native(u"groupEnd", noop)));
        console->set(u"clear", Value::make_heap_ptr(new_native(u"clear", noop)));
        console->set(u"count", Value::make_heap_ptr(new_native(u"count", noop)));
        console->set(u"countReset", Value::make_heap_ptr(new_native(u"countReset", noop)));
        console->set(u"time", Value::make_heap_ptr(new_native(u"time", noop)));
        console->set(u"timeLog", Value::make_heap_ptr(new_native(u"timeLog", noop)));
        console->set(u"timeEnd", Value::make_heap_ptr(new_native(u"timeEnd", noop)));
        console->set(u"profile", Value::make_heap_ptr(new_native(u"profile", noop)));
        console->set(u"profileEnd", Value::make_heap_ptr(new_native(u"profileEnd", noop)));
        console->set(u"timeStamp", Value::make_heap_ptr(new_native(u"timeStamp", noop)));
        global_->define(u"console", Value::make_heap_ptr(console));
    }

    // ---- Math ----
    {
        JSObject* m = new_object();
        m->set(u"PI", Value::make_double(M_PI));
        m->set(u"E", Value::make_double(M_E));
        auto un = [&](const char* n, double(*fn)(double)) {
            m->set(u16(n), Value::make_heap_ptr(new_native(u16(n),
                [fn](Interpreter& in, Value, std::vector<Value>& a) {
                    return Value::make_double(fn(in.to_number(arg(a, 0)))); })));
        };
        un("abs", [](double x){ return std::abs(x); });
        un("floor", [](double x){ return std::floor(x); });
        un("ceil", [](double x){ return std::ceil(x); });
        un("round", [](double x){ return std::floor(x + 0.5); });
        un("trunc", [](double x){ return std::trunc(x); });
        un("sqrt", [](double x){ return std::sqrt(x); });
        un("sin", [](double x){ return std::sin(x); });
        un("cos", [](double x){ return std::cos(x); });
        un("log", [](double x){ return std::log(x); });
        un("log2", [](double x){ return std::log2(x); });
        un("log10", [](double x){ return std::log10(x); });
        un("exp", [](double x){ return std::exp(x); });
        un("cbrt", [](double x){ return std::cbrt(x); });
        un("tan", [](double x){ return std::tan(x); });
        un("asin", [](double x){ return std::asin(x); });
        un("acos", [](double x){ return std::acos(x); });
        un("atan", [](double x){ return std::atan(x); });
        un("sinh", [](double x){ return std::sinh(x); });
        un("cosh", [](double x){ return std::cosh(x); });
        un("tanh", [](double x){ return std::tanh(x); });
        un("sign", [](double x){ return (x > 0) - (x < 0) + 0.0; });
        m->set(u"LN2", Value::make_double(0.6931471805599453));
        m->set(u"LN10", Value::make_double(2.302585092994046));
        m->set(u"SQRT2", Value::make_double(1.4142135623730951));
        m->set(u"atan2", Value::make_heap_ptr(new_native(u"atan2", [](Interpreter& in, Value, std::vector<Value>& a) {
            return Value::make_double(std::atan2(in.to_number(arg(a, 0)), in.to_number(arg(a, 1)))); })));
        m->set(u"hypot", Value::make_heap_ptr(new_native(u"hypot", [](Interpreter& in, Value, std::vector<Value>& a) {
            double s = 0; for (auto& v : a) { double d = in.to_number(v); s += d * d; } return Value::make_double(std::sqrt(s)); })));
        m->set(u"max", Value::make_heap_ptr(new_native(u"max", [](Interpreter& in, Value, std::vector<Value>& a) {
            double r = -std::numeric_limits<double>::infinity();
            for (auto& v : a) r = std::max(r, in.to_number(v));
            return Value::make_double(r); })));
        m->set(u"min", Value::make_heap_ptr(new_native(u"min", [](Interpreter& in, Value, std::vector<Value>& a) {
            double r = std::numeric_limits<double>::infinity();
            for (auto& v : a) r = std::min(r, in.to_number(v));
            return Value::make_double(r); })));
        m->set(u"pow", Value::make_heap_ptr(new_native(u"pow", [](Interpreter& in, Value, std::vector<Value>& a) {
            return Value::make_double(std::pow(in.to_number(arg(a, 0)), in.to_number(arg(a, 1)))); })));
        m->set(u"random", Value::make_heap_ptr(new_native(u"random", [](Interpreter&, Value, std::vector<Value>&) {
            return Value::make_double(static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0)); })));
        global_->define(u"Math", Value::make_heap_ptr(m));
    }

    // ---- Object ----
    {
        JSFunction* Obj = new_native(u"Object", [](Interpreter& in, Value, std::vector<Value>& a) {
            Value value = arg(a, 0);
            if (value.is_undefined() || value.is_null())
                return Value::make_heap_ptr(in.new_object());
            if (value.is_heap_ptr() &&
                value.as_heap_ptr()->kind != HeapObject::kJSString &&
                value.as_heap_ptr()->kind != HeapObject::kJSBigInt &&
                value.as_heap_ptr()->kind != HeapObject::kJSSymbol)
                return value;
            JSObject* wrapper = in.new_object();
            if (value.is_heap_ptr() && value.as_heap_ptr()->kind == HeapObject::kJSString)
                wrapper->proto = in.string_proto();
            else if (value.is_heap_ptr() && value.as_heap_ptr()->kind == HeapObject::kJSBigInt)
                wrapper->proto = in.bigint_proto();
            else if (value.is_heap_ptr() && value.as_heap_ptr()->kind == HeapObject::kJSSymbol)
                wrapper->proto = in.symbol_proto();
            else if (value.is_bool())
                wrapper->proto = in.boolean_proto();
            else if (value.is_int32() || value.is_double())
                wrapper->proto = in.number_proto();
            wrapper->set(u"%primitive%", value, false);
            return Value::make_heap_ptr(wrapper);
        });
        Obj->set(u"keys", Value::make_heap_ptr(new_native(u"keys", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array();
            Value v = arg(a, 0);
            if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* array = static_cast<JSArray*>(v.as_heap_ptr());
                for (size_t i = 0; i < array->elements.size(); ++i)
                    if (array->has_index(i)) out->append(in.str(std::to_string(i)));
                for (auto& k : array->own_enumerable_keys())
                    out->append(in.str(narrow(k)));
            } else if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSObject) {
                for (auto& k : static_cast<JSObject*>(v.as_heap_ptr())->own_enumerable_keys())
                    out->append(in.str(narrow(k)));
            }
            else if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSFunction)
                for (const auto& p : static_cast<JSFunction*>(v.as_heap_ptr())->props)
                    if (p.enumerable) out->elements.push_back(in.str(narrow(p.key)));
            return Value::make_heap_ptr(out); })));
        Obj->set(u"values", Value::make_heap_ptr(new_native(u"values", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array();
            Value v = arg(a, 0);
            if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* array = static_cast<JSArray*>(v.as_heap_ptr());
                for (size_t i = 0; i < array->elements.size(); ++i)
                    if (array->has_index(i)) out->append(array->elements[i]);
                for (auto& p : array->props) if (p.enumerable) out->append(p.value);
            } else if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSObject)
                for (auto& p : static_cast<JSObject*>(v.as_heap_ptr())->props) if (p.enumerable) out->elements.push_back(p.value);
            return Value::make_heap_ptr(out); })));
        Obj->set(u"entries", Value::make_heap_ptr(new_native(u"entries", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array();
            Value v = arg(a, 0);
            if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* array = static_cast<JSArray*>(v.as_heap_ptr());
                auto append_pair = [&](std::u16string key, Value value) {
                    JSArray* pair = in.new_array();
                    pair->elements = {in.str(narrow(key)), value};
                    out->append(Value::make_heap_ptr(pair));
                };
                for (size_t i = 0; i < array->elements.size(); ++i)
                    if (array->has_index(i))
                        append_pair(u16(std::to_string(i)), array->elements[i]);
                for (auto& p : array->props)
                    if (p.enumerable) append_pair(p.key, p.value);
            } else if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSObject)
                for (auto& p : static_cast<JSObject*>(v.as_heap_ptr())->props) if (p.enumerable) {
                    JSArray* pair = in.new_array();
                    pair->elements = {in.str(narrow(p.key)), p.value};
                    out->elements.push_back(Value::make_heap_ptr(pair));
                }
            return Value::make_heap_ptr(out); })));
        Obj->set(u"fromEntries", Value::make_heap_ptr(new_native(u"fromEntries", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSObject* out = in.new_object();
            for (Value pair : in.to_values(arg(a, 0))) {
                std::vector<Value> kv = in.to_values(pair);
                if (!kv.empty())
                    out->set(in.to_property_key(kv[0]),
                             kv.size() > 1 ? kv[1]
                                           : Value::make_undefined());
            }
            return Value::make_heap_ptr(out); })));
        Obj->set(u"hasOwn", Value::make_heap_ptr(new_native(
            u"hasOwn",
            [](Interpreter& in, Value, std::vector<Value>& a) {
                Value object = arg(a, 0);
                if (object.is_null() || object.is_undefined())
                    in.throw_error(
                        u"TypeError",
                        u"Cannot convert undefined or null to object");
                return Value::make_bool(has_own_property(
                    object, in.to_property_key(arg(a, 1))));
            },
            2)));
        Obj->set(u"getPrototypeOf", Value::make_heap_ptr(new_native(u"getPrototypeOf", [](Interpreter&, Value, std::vector<Value>& a) -> Value {
            Value v = arg(a, 0);
            if (JSObject* vo = as_proto_object(v)) {
                JSObject* p = vo->proto;
                return p ? Value::make_heap_ptr(p) : Value::make_null();
            }
            return Value::make_null(); })));
        Obj->set(u"create", Value::make_heap_ptr(new_native(u"create", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSObject* o = in.new_object();
            Value proto = arg(a, 0);
            o->proto = (proto.is_heap_ptr() && proto.as_heap_ptr()->kind == HeapObject::kJSObject)
                           ? static_cast<JSObject*>(proto.as_heap_ptr()) : nullptr;
            return Value::make_heap_ptr(o); })));
        Obj->set(u"getOwnPropertyNames", Value::make_heap_ptr(new_native(u"getOwnPropertyNames", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array(); Value v = arg(a, 0);
            if (!v.is_heap_ptr()) return Value::make_heap_ptr(out);
            HeapObject* h = v.as_heap_ptr();
            if (h->kind == HeapObject::kJSArray) {
                auto* arr = static_cast<JSArray*>(h);
                for (size_t i = 0; i < arr->elements.size(); ++i)
                    if (arr->has_index(i))
                        out->elements.push_back(in.str(std::to_string(i)));
                out->elements.push_back(in.str(std::string("length")));
                for (auto& p : arr->props) out->elements.push_back(in.str(narrow(p.key)));
            } else if (h->kind == HeapObject::kJSObject || h->kind == HeapObject::kJSMap ||
                       h->kind == HeapObject::kJSSet || h->kind == HeapObject::kJSPromise ||
                       h->kind == HeapObject::kJSGenerator) {
                for (auto& p : static_cast<JSObject*>(h)->props) out->elements.push_back(in.str(narrow(p.key)));
            } else if (h->kind == HeapObject::kJSFunction) {
                auto* fn = static_cast<JSFunction*>(h);
                out->elements.push_back(in.str(std::string("length")));
                out->elements.push_back(in.str(std::string("name")));
                for (auto& p : fn->props) out->elements.push_back(in.str(narrow(p.key)));
            }
            return Value::make_heap_ptr(out); }, 1)));
        Obj->set(u"setPrototypeOf", Value::make_heap_ptr(new_native(u"setPrototypeOf", [](Interpreter&, Value, std::vector<Value>& a) -> Value {
            Value v = arg(a, 0), proto = arg(a, 1);
            if (JSObject* vo = as_proto_object(v))
                vo->proto = as_proto_object(proto);
            return v; }, 2)));
        // Object.defineProperty(obj, key, descriptor): supports value + get/set.
        Obj->set(u"defineProperty", Value::make_heap_ptr(new_native(u"defineProperty", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value result = arg(a, 0), v = result, desc = arg(a, 2);
            if (!v.is_heap_ptr()) return v;
            HeapObject* target = v.as_heap_ptr();
            while (target->kind == HeapObject::kJSProxy) {
                auto* proxy = static_cast<JSProxy*>(target);
                Value trap = in.get_prop_public(
                    proxy->handler, u"defineProperty");
                if (in.is_callable(trap)) {
                    Value key_value = arg(a, 1);
                    if (!(key_value.is_heap_ptr() &&
                          key_value.as_heap_ptr()->kind ==
                              HeapObject::kJSSymbol))
                        key_value =
                            in.str(in.to_property_key(key_value));
                    std::vector<Value> arguments{
                        proxy->target, key_value, desc};
                    if (!in.to_bool(in.call(
                            trap, proxy->handler, arguments)))
                        in.throw_error(
                            u"TypeError",
                            u"Proxy defineProperty trap returned false");
                    return result;
                }
                v = proxy->target;
                if (!v.is_heap_ptr()) return result;
                target = v.as_heap_ptr();
            }
            if (target->kind != HeapObject::kJSObject &&
                target->kind != HeapObject::kJSArray &&
                target->kind != HeapObject::kJSFunction)
                return result;
            std::u16string key = in.to_property_key(arg(a, 1));
            if (desc.is_heap_ptr() && desc.as_heap_ptr()->kind == HeapObject::kJSObject) {
                JSObject* d = static_cast<JSObject*>(desc.as_heap_ptr());
                if (target->kind == HeapObject::kJSArray && key == u"length") {
                    auto* array = static_cast<JSArray*>(target);
                    if (d->has_own(u"value") && array->length_writable) {
                        size_t length = static_cast<size_t>(
                            std::max(0.0, in.to_number(d->get(u"value"))));
                        array->resize_length(length, false);
                    }
                    if (d->has_own(u"writable"))
                        array->length_writable = in.to_bool(d->get(u"writable"));
                    return result;
                }
                if (target->kind == HeapObject::kJSArray) {
                    size_t index;
                    if (parse_index_u16(key, index) && d->has_own(u"value")) {
                        static_cast<JSArray*>(target)->set_index(index, d->get(u"value"));
                        return result;
                    }
                }
                const bool has_get = d->has_own(u"get");
                const bool has_set = d->has_own(u"set");
                const bool has_value = d->has_own(u"value");
                const bool has_writable = d->has_own(u"writable");
                if ((has_get || has_set) &&
                    (has_value || has_writable)) {
                    in.throw_error(
                        u"TypeError",
                        u"Invalid property descriptor");
                }
                JSObject* object =
                    static_cast<JSObject*>(target);
                Property* property = object->find_own(key);
                if (!property) {
                    if (!object->extensible) return result;
                    object->props.push_back(Property{
                        key, Value::make_undefined(), false,
                        false, {}, {}, false, false});
                    property = &object->props.back();
                }
                if (d->has_own(u"enumerable"))
                    property->enumerable =
                        in.to_bool(d->get(u"enumerable"));
                if (d->has_own(u"configurable"))
                    property->configurable =
                        in.to_bool(d->get(u"configurable"));
                if (has_get || has_set) {
                    if (!property->is_accessor) {
                        property->is_accessor = true;
                        property->value = Value::make_undefined();
                        property->writable = false;
                        property->getter = Value::make_undefined();
                        property->setter = Value::make_undefined();
                    }
                    if (has_get)
                        property->getter = d->get(u"get");
                    if (has_set)
                        property->setter = d->get(u"set");
                } else {
                    if (property->is_accessor) {
                        property->is_accessor = false;
                        property->getter = Value::make_undefined();
                        property->setter = Value::make_undefined();
                        property->value = Value::make_undefined();
                        property->writable = false;
                    }
                    if (has_value)
                        property->value = d->get(u"value");
                    if (has_writable)
                        property->writable =
                            in.to_bool(d->get(u"writable"));
                }
            }
            return result; }, 3)));
        Obj->set(u"getOwnPropertyDescriptor", Value::make_heap_ptr(new_native(u"getOwnPropertyDescriptor", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value v = arg(a, 0); if (!v.is_heap_ptr()) return Value::make_undefined();
            std::u16string key = in.to_property_key(arg(a, 1));
            HeapObject* h = v.as_heap_ptr();
            while (h->kind == HeapObject::kJSProxy) {
                auto* proxy = static_cast<JSProxy*>(h);
                Value trap = in.get_prop_public(
                    proxy->handler, u"getOwnPropertyDescriptor");
                if (in.is_callable(trap)) {
                    Value key_value = arg(a, 1);
                    if (!(key_value.is_heap_ptr() &&
                          key_value.as_heap_ptr()->kind ==
                              HeapObject::kJSSymbol))
                        key_value = in.str(key);
                    std::vector<Value> arguments{
                        proxy->target, key_value};
                    return in.call(
                        trap, proxy->handler, arguments);
                }
                v = proxy->target;
                if (!v.is_heap_ptr())
                    return Value::make_undefined();
                h = v.as_heap_ptr();
            }
            // Functions expose `name`/`length` as own (non-enumerable, non-writable,
            // configurable) data properties, plus any explicit own props.
            if (h->kind == HeapObject::kJSFunction) {
                auto* fn = static_cast<JSFunction*>(h);
                JSObject* d = in.new_object();
                if (key == u"name" || key == u"length") {
                    d->set(u"value", key == u"name" ? in.str(narrow(fn->name))
                                                    : Value::make_int32(static_cast<int32_t>(fn->arity)));
                    d->set(u"writable", Value::make_bool(false));
                    d->set(u"enumerable", Value::make_bool(false));
                    d->set(u"configurable", Value::make_bool(true));
                    return Value::make_heap_ptr(d);
                }
                Property* p = fn->find_own(key); if (!p) return Value::make_undefined();
                if (p->is_accessor) {
                    d->set(u"get", p->getter);
                    d->set(u"set", p->setter);
                } else {
                    d->set(u"value", p->value);
                    d->set(
                        u"writable",
                        Value::make_bool(p->writable));
                }
                d->set(u"enumerable", Value::make_bool(p->enumerable));
                d->set(
                    u"configurable",
                    Value::make_bool(p->configurable));
                return Value::make_heap_ptr(d);
            }
            if (h->kind != HeapObject::kJSObject && h->kind != HeapObject::kJSArray) return Value::make_undefined();
            JSObject* o = static_cast<JSObject*>(h);
            if (h->kind == HeapObject::kJSArray && key == u"length") {
                auto* array = static_cast<JSArray*>(h);
                JSObject* d = in.new_object();
                d->set(u"value", Value::make_double(static_cast<double>(array->elements.size())));
                d->set(u"writable", Value::make_bool(array->length_writable));
                d->set(u"enumerable", Value::make_bool(false));
                d->set(u"configurable", Value::make_bool(false));
                return Value::make_heap_ptr(d);
            }
            if (h->kind == HeapObject::kJSArray) {
                auto* array = static_cast<JSArray*>(h);
                size_t index;
                if (parse_index_u16(key, index)) {
                    if (!array->has_index(index)) return Value::make_undefined();
                    JSObject* d = in.new_object();
                    d->set(u"value", array->elements[index]);
                    d->set(u"writable", Value::make_bool(true));
                    d->set(u"enumerable", Value::make_bool(true));
                    d->set(u"configurable", Value::make_bool(true));
                    return Value::make_heap_ptr(d);
                }
            }
            Property* p = o->find_own(key); if (!p) return Value::make_undefined();
            JSObject* d = in.new_object();
            if (p->is_accessor) { d->set(u"get", p->getter); d->set(u"set", p->setter); }
            else {
                d->set(u"value", p->value);
                d->set(
                    u"writable",
                    Value::make_bool(p->writable));
            }
            d->set(u"enumerable", Value::make_bool(p->enumerable));
            d->set(
                u"configurable",
                Value::make_bool(p->configurable));
            return Value::make_heap_ptr(d); }, 2)));
        Obj->set(u"assign", Value::make_heap_ptr(new_native(u"assign", [](Interpreter& in, Value, std::vector<Value>& a) {
            Value target = arg(a, 0);
            if (target.is_null() || target.is_undefined())
                in.throw_error(
                    u"TypeError",
                    u"Cannot convert undefined or null to object");
            for (size_t i = 1; i < a.size(); ++i) {
                Value source_value = a[i];
                if (source_value.is_null() ||
                    source_value.is_undefined() ||
                    !source_value.is_heap_ptr())
                    continue;
                HeapObject* source = source_value.as_heap_ptr();
                if (source->kind == HeapObject::kJSString) {
                    const auto& text =
                        static_cast<JSString*>(source)->data;
                    for (size_t index = 0;
                         index < text.size(); ++index) {
                        std::u16string key =
                            u16(std::to_string(index));
                        in.set_prop_public(
                            target, key,
                            in.get_prop_public(source_value, key));
                    }
                    continue;
                }
                if (source->kind == HeapObject::kJSArray) {
                    auto* array = static_cast<JSArray*>(source);
                    for (size_t index = 0;
                         index < array->elements.size(); ++index) {
                        if (!array->has_index(index)) continue;
                        std::u16string key =
                            u16(std::to_string(index));
                        in.set_prop_public(
                            target, key,
                            in.get_prop_public(source_value, key));
                    }
                } else if (source->kind ==
                           HeapObject::kTypedArray) {
                    auto* typed =
                        static_cast<JSTypedArray*>(source);
                    for (size_t index = 0;
                         index < typed->length; ++index) {
                        std::u16string key =
                            u16(std::to_string(index));
                        in.set_prop_public(
                            target, key,
                            in.get_prop_public(source_value, key));
                    }
                }
                switch (source->kind) {
                    case HeapObject::kJSObject:
                    case HeapObject::kJSArray:
                    case HeapObject::kJSFunction:
                    case HeapObject::kJSPromise:
                    case HeapObject::kJSMap:
                    case HeapObject::kJSSet:
                    case HeapObject::kJSGenerator:
                    case HeapObject::kTypedArray:
                    case HeapObject::kDataView:
                    case HeapObject::kArrayBuffer: {
                        auto keys =
                            static_cast<JSObject*>(source)
                                ->own_enumerable_keys();
                        for (const auto& key : keys)
                            in.set_prop_public(
                                target, key,
                                in.get_prop_public(
                                    source_value, key));
                        break;
                    }
                    default:
                        break;
                }
            }
            return target; })));
        // Object.is(a, b): SameValue (NaN equal to itself; +0 != -0).
        Obj->set(u"is", Value::make_heap_ptr(new_native(u"is", [](Interpreter&, Value, std::vector<Value>& a) {
            Value x = arg(a, 0), y = arg(a, 1);
            if ((x.is_double() || x.is_int32()) && (y.is_double() || y.is_int32())) {
                double dx = x.is_int32() ? x.as_int32() : x.as_double();
                double dy = y.is_int32() ? y.as_int32() : y.as_double();
                if (std::isnan(dx) && std::isnan(dy)) return Value::make_bool(true);
                if (dx == 0 && dy == 0) return Value::make_bool(std::signbit(dx) == std::signbit(dy));
                return Value::make_bool(dx == dy);
            }
            return Value::make_bool(x == y); }, 2)));
        // Object.defineProperties(obj, descs)
        Obj->set(u"defineProperties", Value::make_heap_ptr(new_native(u"defineProperties", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value v = arg(a, 0), descs = arg(a, 1);
            if (!v.is_heap_ptr() || !descs.is_heap_ptr()) return v;
            JSObject* o = static_cast<JSObject*>(v.as_heap_ptr());
            if (descs.as_heap_ptr()->kind != HeapObject::kJSObject) return v;
            for (auto& p : static_cast<JSObject*>(descs.as_heap_ptr())->props) {
                if (!p.enumerable || !p.value.is_heap_ptr()) continue;
                JSObject* d = static_cast<JSObject*>(p.value.as_heap_ptr());
                Value g = d->get(u"get"), s = d->get(u"set");
                bool isfn = (g.is_heap_ptr() && g.as_heap_ptr()->kind == HeapObject::kJSFunction) ||
                            (s.is_heap_ptr() && s.as_heap_ptr()->kind == HeapObject::kJSFunction);
                if (isfn) {
                    bool enumerable = in.to_bool(d->get(u"enumerable"));
                    if (g.is_heap_ptr()) o->define_accessor(p.key, g, false, enumerable);
                    if (s.is_heap_ptr()) o->define_accessor(p.key, s, true, enumerable);
                }
                else o->set(p.key, d->get(u"value"), in.to_bool(d->get(u"enumerable")));
            }
            return v; }, 2)));
        // Object.getOwnPropertyDescriptors(obj)
        Obj->set(u"getOwnPropertyDescriptors", Value::make_heap_ptr(new_native(u"getOwnPropertyDescriptors", [Obj](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSObject* out = in.new_object();
            Root keep(in, Value::make_heap_ptr(out));  // out unrooted across getOwnPropertyDescriptor calls
            Value v = arg(a, 0);
            if (v.is_heap_ptr() && (v.as_heap_ptr()->kind == HeapObject::kJSObject || v.as_heap_ptr()->kind == HeapObject::kJSArray)) {
                Value gopd = Obj->get(u"getOwnPropertyDescriptor");
                for (auto& p : static_cast<JSObject*>(v.as_heap_ptr())->props) {
                    std::vector<Value> args{v, in.str(narrow(p.key))};
                    out->set(p.key, in.call(gopd, Value::make_undefined(), args));
                }
            }
            return Value::make_heap_ptr(out); }, 1)));
        Obj->set(u"getOwnPropertySymbols", Value::make_heap_ptr(new_native(u"getOwnPropertySymbols", [](Interpreter& in, Value, std::vector<Value>&) {
            return Value::make_heap_ptr(in.new_array()); }, 1)));  // symbols-as-keys not modelled
        // ---- integrity levels ----
        auto as_obj = [](Value v) -> JSObject* {
            return (v.is_heap_ptr() && (v.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                        v.as_heap_ptr()->kind == HeapObject::kJSArray))
                       ? static_cast<JSObject*>(v.as_heap_ptr()) : nullptr;
        };
        Obj->set(u"isExtensible", Value::make_heap_ptr(new_native(u"isExtensible", [as_obj](Interpreter&, Value, std::vector<Value>& a) {
            JSObject* o = as_obj(arg(a, 0)); return Value::make_bool(o && o->extensible); }, 1)));
        Obj->set(u"preventExtensions", Value::make_heap_ptr(new_native(u"preventExtensions", [as_obj](Interpreter&, Value, std::vector<Value>& a) -> Value {
            if (JSObject* o = as_obj(arg(a, 0))) o->extensible = false;
            return arg(a, 0); }, 1)));
        Obj->set(u"seal", Value::make_heap_ptr(new_native(u"seal", [as_obj](Interpreter&, Value, std::vector<Value>& a) -> Value {
            if (JSObject* o = as_obj(arg(a, 0))) { o->extensible = false; o->sealed = true; }
            return arg(a, 0); }, 1)));
        Obj->set(u"isSealed", Value::make_heap_ptr(new_native(u"isSealed", [as_obj](Interpreter&, Value, std::vector<Value>& a) {
            JSObject* o = as_obj(arg(a, 0)); return Value::make_bool(o ? (o->sealed || o->frozen) : true); }, 1)));
        Obj->set(u"freeze", Value::make_heap_ptr(new_native(u"freeze", [as_obj](Interpreter&, Value, std::vector<Value>& a) -> Value {
            if (JSObject* o = as_obj(arg(a, 0))) { o->extensible = false; o->sealed = true; o->frozen = true; }
            return arg(a, 0); }, 1)));
        Obj->set(u"isFrozen", Value::make_heap_ptr(new_native(u"isFrozen", [as_obj](Interpreter&, Value, std::vector<Value>& a) {
            JSObject* o = as_obj(arg(a, 0)); return Value::make_bool(o ? o->frozen : true); }, 1)));
        global_->define(u"Object", Value::make_heap_ptr(Obj));
    }

    // Internal helper for object rest patterns `{...rest}`: copies the source's
    // own enumerable properties except for the keys already destructured. Bound
    // under a non-identifier name so user code cannot shadow it.
    global_->define(u"%CopyRest%", Value::make_heap_ptr(new_native(u"", [](Interpreter& in, Value, std::vector<Value>& a) {
        JSObject* out = in.new_object();
        Value src = arg(a, 0);
        if (src.is_heap_ptr() && (src.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                  src.as_heap_ptr()->kind == HeapObject::kJSArray)) {
            auto* o = static_cast<JSObject*>(src.as_heap_ptr());
            for (auto& p : o->props) {
                if (!p.enumerable) continue;
                bool taken = false;
                for (size_t i = 1; i < a.size(); ++i)
                    if (a[i].is_heap_ptr() && a[i].as_heap_ptr()->kind == HeapObject::kJSString &&
                        static_cast<JSString*>(a[i].as_heap_ptr())->data == p.key) { taken = true; break; }
                if (!taken) out->set(p.key, p.value);
            }
        }
        return Value::make_heap_ptr(out); })));

    // ---- Array ----
    {
        JSFunction* Arr = new_native(u"Array", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array();
            if (a.size() == 1 && (a[0].is_int32() || a[0].is_double()))
                out->resize_length(static_cast<size_t>(in.to_number(a[0])), false);
            else
                for (Value value : a) out->append(value);
            return Value::make_heap_ptr(out); });
        Arr->set(u"isArray", Value::make_heap_ptr(new_native(u"isArray", [](Interpreter&, Value, std::vector<Value>& a) {
            return Value::make_bool(arg(a, 0).is_heap_ptr() && arg(a, 0).as_heap_ptr()->kind == HeapObject::kJSArray); })));
        Arr->set(u"from", Value::make_heap_ptr(new_native(u"from", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array();
            Root keep(in, Value::make_heap_ptr(out));  // out unrooted across iterator/mapfn callbacks
            std::vector<Value> vals = in.to_values(arg(a, 0));
            Value mapfn = arg(a, 1);
            bool has_map = mapfn.is_heap_ptr() && mapfn.as_heap_ptr()->kind == HeapObject::kJSFunction;
            for (size_t i = 0; i < vals.size(); ++i) {
                if (has_map) {
                    std::vector<Value> ma{vals[i], Value::make_int32(static_cast<int32_t>(i))};
                    out->elements.push_back(in.call(mapfn, Value::make_undefined(), ma));
                } else out->elements.push_back(vals[i]);
            }
            return Value::make_heap_ptr(out); })));
        Arr->set(u"of", Value::make_heap_ptr(new_native(u"of", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array(); out->elements = a; return Value::make_heap_ptr(out); })));
        global_->define(u"Array", Value::make_heap_ptr(Arr));
    }

    // ---- Map ----
    {
        auto as_map = [](Value v) -> JSMap* {
            return (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSMap)
                       ? static_cast<JSMap*>(v.as_heap_ptr()) : nullptr;
        };
        auto find = [](Interpreter& in, JSMap* m, Value key) -> int {
            for (size_t i = 0; i < m->entries.size(); ++i)
                if (in.strict_equals(m->entries[i].first, key)) return static_cast<int>(i);
            return -1;
        };
        method(map_proto_, "set", [as_map, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSMap* m = as_map(t); if (!m) return t;
            int i = find(in, m, arg(a, 0));
            if (i >= 0) m->entries[i].second = arg(a, 1);
            else m->entries.emplace_back(arg(a, 0), arg(a, 1));
            return t;  // chainable
        }, 2);
        method(map_proto_, "get", [as_map, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSMap* m = as_map(t); if (!m) return Value::make_undefined();
            int i = find(in, m, arg(a, 0));
            return i >= 0 ? m->entries[i].second : Value::make_undefined();
        }, 1);
        method(map_proto_, "has", [as_map, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSMap* m = as_map(t); return Value::make_bool(m && find(in, m, arg(a, 0)) >= 0);
        }, 1);
        method(map_proto_, "delete", [as_map, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSMap* m = as_map(t); if (!m) return Value::make_bool(false);
            int i = find(in, m, arg(a, 0));
            if (i < 0) return Value::make_bool(false);
            m->entries.erase(m->entries.begin() + i); return Value::make_bool(true);
        }, 1);
        method(map_proto_, "clear", [as_map](Interpreter&, Value t, std::vector<Value>&) -> Value {
            if (JSMap* m = as_map(t)) m->entries.clear();
            return Value::make_undefined();
        });
        method(map_proto_, "forEach", [as_map](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSMap* m = as_map(t); Value cb = arg(a, 0); if (!m) return Value::make_undefined();
            for (auto& [k, v] : m->entries) { std::vector<Value> ca{v, k, t}; in.call(cb, arg(a, 1), ca); }
            return Value::make_undefined();
        }, 1);
        method(map_proto_, "keys", [as_map](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            JSArray* out = in.new_array(); if (JSMap* m = as_map(t)) for (auto& e : m->entries) out->elements.push_back(e.first);
            return array_iterator(in, out);
        });
        method(map_proto_, "values", [as_map](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            JSArray* out = in.new_array(); if (JSMap* m = as_map(t)) for (auto& e : m->entries) out->elements.push_back(e.second);
            return array_iterator(in, out);
        });
        method(map_proto_, "entries", [as_map](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            JSArray* out = in.new_array();
            if (JSMap* m = as_map(t)) for (auto& e : m->entries) {
                JSArray* pair = in.new_array(); pair->elements = {e.first, e.second};
                out->elements.push_back(Value::make_heap_ptr(pair));
            }
            return array_iterator(in, out);
        });
        map_proto_->set(
            u"@@iterator", map_proto_->get(u"entries"), false);
        JSFunction* Map = new_native(u"Map", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSMap* m = in.new_map();
            Value init = arg(a, 0);
            if (!init.is_undefined() && !init.is_null())
                for (Value pair : in.to_values(init)) {
                    std::vector<Value> kv = in.to_values(pair);
                    m->entries.emplace_back(kv.size() > 0 ? kv[0] : Value::make_undefined(),
                                            kv.size() > 1 ? kv[1] : Value::make_undefined());
                }
            return Value::make_heap_ptr(m);
        });
        Map->set(u"prototype", Value::make_heap_ptr(map_proto_), false);
        map_proto_->set(u"constructor", Value::make_heap_ptr(Map), false);
        global_->define(u"Map", Value::make_heap_ptr(Map));
        global_->define(u"WeakMap", Value::make_heap_ptr(Map));  // no GC-weakness; API-compatible
    }

    // ---- Set ----
    {
        auto as_set = [](Value v) -> JSSet* {
            return (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSSet)
                       ? static_cast<JSSet*>(v.as_heap_ptr()) : nullptr;
        };
        auto find = [](Interpreter& in, JSSet* s, Value val) -> int {
            for (size_t i = 0; i < s->items.size(); ++i)
                if (in.strict_equals(s->items[i], val)) return static_cast<int>(i);
            return -1;
        };
        method(set_proto_, "add", [as_set, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSSet* s = as_set(t); if (!s) return t;
            if (find(in, s, arg(a, 0)) < 0) s->items.push_back(arg(a, 0));
            return t;  // chainable
        }, 1);
        method(set_proto_, "has", [as_set, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSSet* s = as_set(t); return Value::make_bool(s && find(in, s, arg(a, 0)) >= 0);
        }, 1);
        method(set_proto_, "delete", [as_set, find](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSSet* s = as_set(t); if (!s) return Value::make_bool(false);
            int i = find(in, s, arg(a, 0)); if (i < 0) return Value::make_bool(false);
            s->items.erase(s->items.begin() + i); return Value::make_bool(true);
        }, 1);
        method(set_proto_, "clear", [as_set](Interpreter&, Value t, std::vector<Value>&) -> Value {
            if (JSSet* s = as_set(t)) s->items.clear();
            return Value::make_undefined();
        });
        method(set_proto_, "forEach", [as_set](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            JSSet* s = as_set(t); Value cb = arg(a, 0); if (!s) return Value::make_undefined();
            for (Value v : s->items) { std::vector<Value> ca{v, v, t}; in.call(cb, arg(a, 1), ca); }
            return Value::make_undefined();
        }, 1);
        JSFunction* set_values = new_native(
            u"values",
            [as_set](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            JSArray* out = in.new_array(); if (JSSet* s = as_set(t)) out->elements = s->items;
            return array_iterator(in, out);
        });
        Value set_values_value = Value::make_heap_ptr(set_values);
        set_proto_->set(u"values", set_values_value, false);
        set_proto_->set(u"keys", set_values_value, false);
        set_proto_->set(u"@@iterator", set_values_value, false);
        method(set_proto_, "entries", [as_set](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            JSArray* out = in.new_array();
            if (JSSet* s = as_set(t)) {
                for (Value value : s->items) {
                    JSArray* pair = in.new_array();
                    pair->elements = {value, value};
                    out->elements.push_back(Value::make_heap_ptr(pair));
                }
            }
            return array_iterator(in, out);
        });
        JSFunction* Set = new_native(u"Set", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSSet* s = in.new_set();
            Value init = arg(a, 0);
            if (!init.is_undefined() && !init.is_null())
                for (Value v : in.to_values(init)) {
                    bool dup = false;
                    for (Value e : s->items) if (in.strict_equals(e, v)) { dup = true; break; }
                    if (!dup) s->items.push_back(v);
                }
            return Value::make_heap_ptr(s);
        });
        Set->set(u"prototype", Value::make_heap_ptr(set_proto_), false);
        set_proto_->set(u"constructor", Value::make_heap_ptr(Set), false);
        global_->define(u"Set", Value::make_heap_ptr(Set));
        global_->define(u"WeakSet", Value::make_heap_ptr(Set));
    }

    // ---- RegExp ----
    {
        JSObject* rproto = new_object();
        auto re_search_on = [](Interpreter& in, Value t, const std::u16string& input, size_t start,
                               regex::Match& mr, bool& g) -> bool {
            std::u16string src; bool ic, ml;
            if (!get_regex(in, t, src, g, ic, ml)) return false;
            mr = regex::search(src, input, start, ic, ml);
            return true;
        };
        rproto->set(u"test", Value::make_heap_ptr(new_native(u"test",
            [re_search_on](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
                std::u16string input = in.to_string(arg(a, 0));
                bool g = false; regex::Match mr;
                if (!t.is_heap_ptr()) return Value::make_bool(false);
                JSObject* o = static_cast<JSObject*>(t.as_heap_ptr());
                size_t start = 0;
                if (in.to_string(o->get(u"flags")).find('g') != std::u16string::npos)
                    start = static_cast<size_t>(in.to_number(o->get(u"lastIndex")));
                if (!re_search_on(in, t, input, start, mr, g)) return Value::make_bool(false);
                if (g) o->set(u"lastIndex", Value::make_int32(mr.matched ? static_cast<int32_t>(mr.end) : 0));
                return Value::make_bool(mr.matched);
            }, 1)));
        rproto->set(u"exec", Value::make_heap_ptr(new_native(u"exec",
            [re_search_on](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
                std::u16string input = in.to_string(arg(a, 0));
                if (!t.is_heap_ptr()) return Value::make_null();
                JSObject* o = static_cast<JSObject*>(t.as_heap_ptr());
                bool g = in.to_string(o->get(u"flags")).find('g') != std::u16string::npos;
                size_t start = g ? static_cast<size_t>(in.to_number(o->get(u"lastIndex"))) : 0;
                bool gg; regex::Match mr;
                if (!re_search_on(in, t, input, start, mr, gg) || !mr.matched) {
                    if (g) o->set(u"lastIndex", Value::make_int32(0));
                    return Value::make_null();
                }
                if (g) o->set(u"lastIndex", Value::make_int32(static_cast<int32_t>(mr.end)));
                JSArray* out = in.new_array();
                for (auto [lo, hi] : mr.groups)
                    out->elements.push_back(lo < 0 ? Value::make_undefined()
                                                   : in.str(narrow(input.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo)))));
                out->set(u"index", Value::make_int32(static_cast<int32_t>(mr.index)));
                out->set(u"input", in.str(narrow(input)));
                return Value::make_heap_ptr(out);
            }, 1)));
        rproto->set(u"toString", Value::make_heap_ptr(new_native(u"toString",
            [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
                if (!t.is_heap_ptr()) return in.str(std::string("/(?:)/"));
                JSObject* o = static_cast<JSObject*>(t.as_heap_ptr());
                return in.str(narrow(u"/" + in.to_string(o->get(u"source")) + u"/" + in.to_string(o->get(u"flags"))));
            })));

        JSFunction* RegExp = new_native(u"RegExp", [rproto](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string src, flags;
            Value first = arg(a, 0);
            std::u16string existing; bool eg, eic, eml;
            if (get_regex(in, first, existing, eg, eic, eml)) {
                src = existing;
                flags = a.size() > 1 ? in.to_string(a[1]) : in.to_string(static_cast<JSObject*>(first.as_heap_ptr())->get(u"flags"));
            } else {
                src = first.is_undefined() ? u"" : in.to_string(first);
                flags = a.size() > 1 ? in.to_string(a[1]) : u"";
            }
            JSObject* re = in.new_object();
            re->set(u"source", in.str(narrow(src)));
            re->set(u"flags", in.str(narrow(flags)));
            re->set(u"global", Value::make_bool(flags.find('g') != std::u16string::npos));
            re->set(u"ignoreCase", Value::make_bool(flags.find('i') != std::u16string::npos));
            re->set(u"multiline", Value::make_bool(flags.find('m') != std::u16string::npos));
            re->set(u"lastIndex", Value::make_int32(0));
            re->set(u"%isRegExp%", Value::make_bool(true), false);
            re->proto = rproto;
            return Value::make_heap_ptr(re);
        });
        RegExp->set(u"prototype", Value::make_heap_ptr(rproto), false);
        rproto->set(u"constructor", Value::make_heap_ptr(RegExp), false);
        global_->define(u"RegExp", Value::make_heap_ptr(RegExp));
    }

    // ---- Generator.prototype (next / return / throw / @@iterator) ----
    {
        auto as_gen = [](Value v) -> JSGenerator* {
            return (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSGenerator)
                       ? static_cast<JSGenerator*>(v.as_heap_ptr()) : nullptr;
        };
        generator_proto_->set(u"next", Value::make_heap_ptr(new_native(u"next",
            [as_gen](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
                JSGenerator* g = as_gen(t);
                if (!g) in.throw_error(u"TypeError", u"not a generator");
                return in.gen_resume(g, arg(a, 0), false, false);
            }, 1)), false);
        generator_proto_->set(u"return", Value::make_heap_ptr(new_native(u"return",
            [as_gen](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
                JSGenerator* g = as_gen(t);
                if (!g) in.throw_error(u"TypeError", u"not a generator");
                return in.gen_resume(g, arg(a, 0), false, true);
            }, 1)), false);
        generator_proto_->set(u"throw", Value::make_heap_ptr(new_native(u"throw",
            [as_gen](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
                JSGenerator* g = as_gen(t);
                if (!g) in.throw_error(u"TypeError", u"not a generator");
                return in.gen_resume(g, arg(a, 0), true, false);
            }, 1)), false);
        generator_proto_->set(u"@@iterator", Value::make_heap_ptr(new_native(u"[Symbol.iterator]",
            [](Interpreter&, Value t, std::vector<Value>&) -> Value { return t; })), false);
    }

    // ---- Symbol ---------------------------------------------------------
    {
        JSFunction* Symbol = new_native(
            u"Symbol",
            [](Interpreter& in, Value,
               std::vector<Value>& a) -> Value {
                std::u16string description =
                    a.empty() || a[0].is_undefined()
                        ? std::u16string()
                        : in.to_string(a[0]);
                return Value::make_heap_ptr(
                    in.new_symbol(std::move(description)));
            },
            0);
        Symbol->constructable = false;
        Symbol->set(
            u"iterator",
            Value::make_heap_ptr(
                new_symbol(u"Symbol.iterator", u"@@iterator")),
            false);
        Symbol->set(
            u"asyncIterator",
            Value::make_heap_ptr(
                new_symbol(u"Symbol.asyncIterator", u"@@asyncIterator")),
            false);
        Symbol->set(
            u"hasInstance",
            Value::make_heap_ptr(
                new_symbol(u"Symbol.hasInstance", u"@@hasInstance")),
            false);
        Symbol->set(
            u"toPrimitive",
            Value::make_heap_ptr(
                new_symbol(u"Symbol.toPrimitive", u"@@toPrimitive")),
            false);
        Symbol->set(
            u"toStringTag",
            Value::make_heap_ptr(
                new_symbol(u"Symbol.toStringTag", u"@@toStringTag")),
            false);
        Symbol->set(
            u"for",
            Value::make_heap_ptr(new_native(
                u"for",
                [this](Interpreter& in, Value,
                       std::vector<Value>& a) -> Value {
                    const std::u16string key =
                        in.to_string(arg(a, 0));
                    auto found = symbol_registry_.find(key);
                    if (found != symbol_registry_.end())
                        return Value::make_heap_ptr(found->second);
                    JSSymbol* symbol = new_symbol(
                        key, u"%symbol.for:" +
                                 u16(std::to_string(next_symbol_id_++)));
                    symbol->registered = true;
                    symbol->registry_key = key;
                    symbol_registry_[key] = symbol;
                    return Value::make_heap_ptr(symbol);
                },
                1)),
            false);
        Symbol->set(
            u"keyFor",
            Value::make_heap_ptr(new_native(
                u"keyFor",
                [](Interpreter& in, Value,
                   std::vector<Value>& a) -> Value {
                    Value value = arg(a, 0);
                    if (!value.is_heap_ptr() ||
                        value.as_heap_ptr()->kind != HeapObject::kJSSymbol)
                        in.throw_error(
                            u"TypeError",
                            u"Symbol.keyFor requires a symbol");
                    auto* symbol =
                        static_cast<JSSymbol*>(value.as_heap_ptr());
                    return symbol->registered
                               ? in.str(symbol->registry_key)
                               : Value::make_undefined();
                },
                1)),
            false);
        global_->define(u"Symbol", Value::make_heap_ptr(Symbol));
    }

    // ---- Function.prototype (call / apply / bind), consulted for every function ----
    {
        method(function_proto_, "call", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            Value thisArg = a.empty() ? Value::make_undefined() : a[0];
            std::vector<Value> rest(a.begin() + (a.empty() ? 0 : 1), a.end());
            return in.call(t, thisArg, rest);
        }, 1);
        method(function_proto_, "apply", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            Value thisArg = arg(a, 0);
            std::vector<Value> args = a.size() > 1 ? in.to_values(a[1]) : std::vector<Value>{};
            return in.call(t, thisArg, args);
        }, 2);
        method(function_proto_, "bind", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            if (!in.is_callable(t))
                in.throw_error(u"TypeError", u"Function.prototype.bind called on incompatible receiver");
            if (std::getenv("MALIBU_TRACE_BIND")) {
                const char* kind = "primitive";
                std::string name;
                if (t.is_undefined()) kind = "undefined";
                else if (t.is_null()) kind = "null";
                else if (t.is_heap_ptr()) {
                    kind = "heap";
                    if (t.as_heap_ptr()->kind == HeapObject::kJSFunction) {
                        kind = "function";
                        name = narrow(static_cast<JSFunction*>(t.as_heap_ptr())->name);
                    }
                }
                std::fprintf(stderr, "[bind] create target=%s:%s raw=%llu argc=%zu\n",
                             kind, name.c_str(),
                             static_cast<unsigned long long>(t.raw), a.size());
                in.trace_call_stack_for_diagnostics("creating bound function");
            }
            // Store target/this/args as own props so the GC traces them (the
            // native std::function's captures are NOT traced).
            JSFunction* bound = in.new_native(u"bound", nullptr);
            bound->set(u"%isBound%", Value::make_bool(true), false);
            bound->set(u"%target%", t);
            bound->set(u"%boundThis%", a.empty() ? Value::make_undefined() : a[0]);
            JSArray* ba = in.new_array();
            ba->elements.assign(a.begin() + (a.empty() ? 0 : 1), a.end());
            bound->set(u"%boundArgs%", Value::make_heap_ptr(ba));
            bound->native = [bound](Interpreter& in2, Value, std::vector<Value>& ca) -> Value {
                if (std::getenv("MALIBU_TRACE_BIND")) {
                    Value target = bound->get(u"%target%");
                    std::fprintf(stderr,
                                 "[bind] invoke fn=%p props=%zu target_raw=%llu target_kind=%d argc=%zu\n",
                                 static_cast<void*>(bound), bound->props.size(),
                                 static_cast<unsigned long long>(target.raw),
                                 target.is_heap_ptr()
                                     ? static_cast<int>(target.as_heap_ptr()->kind)
                                     : -1,
                                 ca.size());
                }
                std::vector<Value> all = static_cast<JSArray*>(bound->get(u"%boundArgs%").as_heap_ptr())->elements;
                all.insert(all.end(), ca.begin(), ca.end());
                return in2.call(bound->get(u"%target%"), bound->get(u"%boundThis%"), all);
            };
            return Value::make_heap_ptr(bound);
        }, 1);
        method(function_proto_, "toString", [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            std::u16string nm;
            if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSFunction)
                nm = static_cast<JSFunction*>(t.as_heap_ptr())->name;
            return in.str(narrow(u"function " + nm + u"() { [native code] }"));
        });
    }

    // ---- Error hierarchy ----
    // Each constructor gets a `.prototype` carrying `constructor`, an inherited
    // (non-enumerable) `name`, an empty `message`, and (on Error.prototype) a
    // `toString`. Subclasses chain their prototype to Error.prototype so that
    // `e instanceof Error`, `e.constructor`, and `e.name` all resolve correctly.
    {
        auto make_error_ctor = [&](const char* name, JSObject* parent_proto) -> JSObject* {
            std::u16string nm = u16(name);
            JSObject* proto = new_object();
            proto->proto = parent_proto ? parent_proto : object_proto_;
            JSFunction* ctor = new_native(nm, [proto](Interpreter& in, Value thisv, std::vector<Value>& a) -> Value {
                JSObject* obj;
                if (thisv.is_heap_ptr() && (thisv.as_heap_ptr()->kind == HeapObject::kJSObject))
                    obj = static_cast<JSObject*>(thisv.as_heap_ptr());
                else { obj = in.new_object(); obj->proto = proto; }   // called without `new`
                if (!a.empty() && !a[0].is_undefined())
                    obj->set(u"message", in.str(narrow(in.to_string(a[0]))), false);
                return Value::make_heap_ptr(obj);
            }, 1);
            ctor->set(u"prototype", Value::make_heap_ptr(proto), false);
            proto->set(u"constructor", Value::make_heap_ptr(ctor), false);
            proto->set(u"name", str(narrow(nm)), false);
            proto->set(u"message", str(""), false);
            global_->define(nm, Value::make_heap_ptr(ctor));
            return proto;
        };
        JSObject* error_proto = make_error_ctor("Error", object_proto_);
        // Error.prototype.toString(): "name" or "name: message".
        error_proto->set(u"toString", Value::make_heap_ptr(new_native(u"toString",
            [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
                if (!t.is_heap_ptr() || t.as_heap_ptr()->kind != HeapObject::kJSObject)
                    in.throw_error(u"TypeError", u"Error.prototype.toString called on non-object");
                JSObject* o = static_cast<JSObject*>(t.as_heap_ptr());
                std::u16string nm = o->has(u"name") ? in.to_string(o->get(u"name")) : u"Error";
                std::u16string msg = o->has(u"message") ? in.to_string(o->get(u"message")) : u"";
                if (nm.empty()) return in.str(msg);
                if (msg.empty()) return in.str(nm);
                return in.str(nm + u": " + msg);
            })), false);
        make_error_ctor("TypeError", error_proto);
        make_error_ctor("RangeError", error_proto);
        make_error_ctor("ReferenceError", error_proto);
        make_error_ctor("SyntaxError", error_proto);
        make_error_ctor("EvalError", error_proto);
        make_error_ctor("URIError", error_proto);
        make_error_ctor("AggregateError", error_proto);
    }

    // ---- global conversion fns ----
    {
        JSFunction* Str = new_native(u"String", [](Interpreter& in, Value, std::vector<Value>& a) {
            if (a.empty()) return in.str(std::u16string());
            Value value = arg(a, 0);
            if (value.is_heap_ptr() &&
                value.as_heap_ptr()->kind == HeapObject::kJSSymbol)
                return in.str(in.symbol_descriptive_string(value));
            return in.str(narrow(in.to_string(value))); }, 1);
        Str->set(u"fromCharCode", Value::make_heap_ptr(new_native(u"fromCharCode", [](Interpreter& in, Value, std::vector<Value>& a) {
            std::u16string s;
            for (auto& v : a) s.push_back(static_cast<char16_t>(static_cast<uint32_t>(in.to_number(v)) & 0xFFFF));
            return in.str(narrow(s)); }, 1)));
        Str->set(u"fromCodePoint", Value::make_heap_ptr(new_native(u"fromCodePoint", [](Interpreter& in, Value, std::vector<Value>& a) {
            std::u16string s;
            for (auto& v : a) {
                uint32_t cp = static_cast<uint32_t>(in.to_number(v));
                if (cp <= 0xFFFF) s.push_back(static_cast<char16_t>(cp));
                else { cp -= 0x10000; s.push_back(static_cast<char16_t>(0xD800 + (cp >> 10))); s.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
            }
            return in.str(narrow(s)); }, 1)));
        // String.raw`...` : cooked-strings object has a `raw` array + substitutions.
        Str->set(u"raw", Value::make_heap_ptr(new_native(u"raw", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value tpl = arg(a, 0);
            Value rawv = in.get_prop_public(tpl, u"raw");
            if (!rawv.is_heap_ptr() || rawv.as_heap_ptr()->kind != HeapObject::kJSArray) return in.str(std::u16string());
            auto* raw = static_cast<JSArray*>(rawv.as_heap_ptr());
            std::u16string out;
            for (size_t i = 0; i < raw->elements.size(); ++i) {
                out += in.to_string(raw->elements[i]);
                if (i + 1 < raw->elements.size() && i + 1 < a.size()) out += in.to_string(a[i + 1]);
            }
            return in.str(narrow(out)); }, 1)));
        global_->define(u"String", Value::make_heap_ptr(Str));
    }
    def("Boolean", [](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_bool(in.to_bool(arg(a, 0))); }, 1);
    {
        JSFunction* Num = new_native(u"Number", [](Interpreter& in, Value, std::vector<Value>& a) {
            if (!a.empty() && a[0].is_heap_ptr() &&
                a[0].as_heap_ptr()->kind == HeapObject::kJSBigInt)
                return Value::make_double(static_cast<JSBigInt*>(a[0].as_heap_ptr())->value.get_d());
            return Value::make_double(a.empty() ? 0.0 : in.to_number(a[0])); });
        Num->arity = 1;
        Num->set(u"MAX_SAFE_INTEGER", Value::make_double(9007199254740991.0));
        Num->set(u"MIN_SAFE_INTEGER", Value::make_double(-9007199254740991.0));
        Num->set(u"MAX_VALUE", Value::make_double(std::numeric_limits<double>::max()));
        Num->set(u"MIN_VALUE", Value::make_double(std::numeric_limits<double>::denorm_min()));
        Num->set(u"EPSILON", Value::make_double(2.220446049250313e-16));
        Num->set(u"POSITIVE_INFINITY", Value::make_double(std::numeric_limits<double>::infinity()));
        Num->set(u"NEGATIVE_INFINITY", Value::make_double(-std::numeric_limits<double>::infinity()));
        Num->set(u"NaN", Value::make_double(std::nan("")));
        Num->set(u"isInteger", Value::make_heap_ptr(new_native(u"isInteger", [](Interpreter&, Value, std::vector<Value>& a) {
            Value v = arg(a, 0); if (!(v.is_int32() || v.is_double())) return Value::make_bool(false);
            double d = v.is_int32() ? v.as_int32() : v.as_double();
            return Value::make_bool(!std::isnan(d) && !std::isinf(d) && d == std::floor(d)); }, 1)));
        Num->set(u"isSafeInteger", Num->get(u"isInteger"));
        Num->set(u"isFinite", Value::make_heap_ptr(new_native(u"isFinite", [](Interpreter&, Value, std::vector<Value>& a) {
            Value v = arg(a, 0); if (!(v.is_int32() || v.is_double())) return Value::make_bool(false);
            double d = v.is_int32() ? v.as_int32() : v.as_double(); return Value::make_bool(!std::isnan(d) && !std::isinf(d)); }, 1)));
        Num->set(u"isNaN", Value::make_heap_ptr(new_native(u"isNaN", [](Interpreter&, Value, std::vector<Value>& a) {
            Value v = arg(a, 0); return Value::make_bool(v.is_double() && std::isnan(v.as_double())); }, 1)));
        Num->set(u"parseFloat", Value::make_heap_ptr(new_native(u"parseFloat", [](Interpreter& in, Value, std::vector<Value>& a) {
            try { return Value::make_double(std::stod(narrow(in.to_string(arg(a, 0))))); } catch (...) { return Value::make_double(std::nan("")); } }, 1)));
        Num->set(u"parseInt", Value::make_heap_ptr(new_native(u"parseInt", [](Interpreter& in, Value, std::vector<Value>& a) {
            std::string s = narrow(in.to_string(arg(a, 0))); int base = a.size() > 1 ? in.to_int32(a[1]) : 10; if (base == 0) base = 10;
            try { return Value::make_double(static_cast<double>(std::stoll(s, nullptr, base))); } catch (...) { return Value::make_double(std::nan("")); } }, 2)));
        global_->define(u"Number", Value::make_heap_ptr(Num));
    }
    {
        JSFunction* BigInt = new_native(u"BigInt",
            [](Interpreter& in, Value, std::vector<Value>& a) {
                return in.to_bigint_constructor(arg(a, 0));
            }, 1);
        BigInt->constructable = false;
        BigInt->set(u"asUintN", Value::make_heap_ptr(new_native(u"asUintN",
            [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                unsigned long bits = to_index(in, arg(a, 0));
                Value input = in.to_bigint(arg(a, 1));
                mpz_class result;
                mpz_fdiv_r_2exp(result.get_mpz_t(),
                                static_cast<JSBigInt*>(input.as_heap_ptr())->value.get_mpz_t(), bits);
                return Value::make_heap_ptr(in.new_bigint(std::move(result)));
            }, 2)));
        BigInt->set(u"asIntN", Value::make_heap_ptr(new_native(u"asIntN",
            [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                unsigned long bits = to_index(in, arg(a, 0));
                Value input = in.to_bigint(arg(a, 1));
                mpz_class result;
                mpz_fdiv_r_2exp(result.get_mpz_t(),
                                static_cast<JSBigInt*>(input.as_heap_ptr())->value.get_mpz_t(), bits);
                if (bits > 0 && mpz_tstbit(result.get_mpz_t(), bits - 1)) {
                    mpz_class modulus = 1;
                    mpz_mul_2exp(modulus.get_mpz_t(), modulus.get_mpz_t(), bits);
                    result -= modulus;
                }
                return Value::make_heap_ptr(in.new_bigint(std::move(result)));
            }, 2)));
        global_->define(u"BigInt", Value::make_heap_ptr(BigInt));
    }
    def("parseInt", [](Interpreter& in, Value, std::vector<Value>& a) {
        std::string s = narrow(in.to_string(arg(a, 0)));
        int base = a.size() > 1 ? in.to_int32(a[1]) : 10; if (base == 0) base = 10;
        try { return Value::make_double(static_cast<double>(std::stoll(s, nullptr, base))); }
        catch (...) { return Value::make_double(std::nan("")); } }, 2);
    def("parseFloat", [](Interpreter& in, Value, std::vector<Value>& a) {
        try { return Value::make_double(std::stod(narrow(in.to_string(arg(a, 0))))); }
        catch (...) { return Value::make_double(std::nan("")); } }, 1);
    def("isNaN", [](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_bool(std::isnan(in.to_number(arg(a, 0)))); }, 1);
    def("isFinite", [](Interpreter& in, Value, std::vector<Value>& a) { double d = in.to_number(arg(a, 0)); return Value::make_bool(!std::isnan(d) && !std::isinf(d)); }, 1);

    // ---- eval / Function ----
    // eval(x): if x is not a string, return it unchanged (per spec); otherwise
    // parse+compile+run the source in the global realm via the Engine's hook.
    // (This is indirect-eval semantics: the program runs in global scope. Direct
    // eval with access to the caller's locals is not modelled.)
    def("eval", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        Value x = arg(a, 0);
        if (!(x.is_heap_ptr() && x.as_heap_ptr()->kind == HeapObject::kJSString)) return x;
        return in.run_eval(in.to_string(x));
    }, 1);
    {
        // Function(p1, p2, ..., pN, body): the leading args are parameter source
        // text (comma-joined), the last is the function body. With no args the
        // result is `function anonymous(){}`. Evaluated via the same hook so the
        // produced closure captures the global realm.
        auto fn_ctor = [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string params, body;
            if (!a.empty()) {
                for (size_t i = 0; i + 1 < a.size(); ++i) {
                    if (i) params += u',';
                    params += in.to_string(a[i]);
                }
                body = in.to_string(a.back());
            }
            std::u16string src = u"(function anonymous(" + params + u"\n) {\n" + body + u"\n})";
            return in.run_eval(src);
        };
        JSFunction* Func = new_native(u"Function", fn_ctor, 1);
        // `Function.prototype` is the shared function prototype (call/apply/bind).
        if (function_proto_)
            Func->set(u"prototype", Value::make_heap_ptr(function_proto_), false);
        global_->define(u"Function", Value::make_heap_ptr(Func));
    }

    // ---- URI encode/decode ----
    {
        auto encode = [](Interpreter& in, std::vector<Value>& a, const char* keep) -> Value {
            std::string s = narrow(in.to_string(arg(a, 0))), out;
            static const char* hex = "0123456789ABCDEF";
            for (unsigned char c : s) {
                bool unreserved = std::isalnum(c) || std::strchr("-_.!~*'()", c) || std::strchr(keep, c);
                if (unreserved) out.push_back(static_cast<char>(c));
                else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
            }
            return in.str(out);
        };
        auto decode = [](Interpreter& in, std::vector<Value>& a) -> Value {
            std::string s = narrow(in.to_string(arg(a, 0))), out;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '%' && i + 2 < s.size()) { out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16))); i += 2; }
                else out.push_back(s[i]);
            }
            return in.str(out);
        };
        def("encodeURIComponent", [encode](Interpreter& in, Value, std::vector<Value>& a) { return encode(in, a, ""); }, 1);
        def("encodeURI", [encode](Interpreter& in, Value, std::vector<Value>& a) { return encode(in, a, ";,/?:@&=+$#"); }, 1);
        def("decodeURIComponent", [decode](Interpreter& in, Value, std::vector<Value>& a) { return decode(in, a); }, 1);
        def("decodeURI", [decode](Interpreter& in, Value, std::vector<Value>& a) { return decode(in, a); }, 1);
    }

    // ---- base64 (btoa / atob), Latin-1 bytes ----
    {
        def("btoa", [](Interpreter& in, Value, std::vector<Value>& a) {
            static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string s = narrow(in.to_string(arg(a, 0))), out;
            for (size_t i = 0; i < s.size(); i += 3) {
                unsigned n = static_cast<unsigned char>(s[i]) << 16;
                if (i + 1 < s.size()) n |= static_cast<unsigned char>(s[i + 1]) << 8;
                if (i + 2 < s.size()) n |= static_cast<unsigned char>(s[i + 2]);
                out.push_back(T[(n >> 18) & 63]); out.push_back(T[(n >> 12) & 63]);
                out.push_back(i + 1 < s.size() ? T[(n >> 6) & 63] : '=');
                out.push_back(i + 2 < s.size() ? T[n & 63] : '=');
            }
            return in.str(out); }, 1);
        def("atob", [](Interpreter& in, Value, std::vector<Value>& a) {
            auto dec = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '+') return 62;
                if (c == '/') return 63;
                return -1;
            };
            std::string s = narrow(in.to_string(arg(a, 0))), out; int val = 0, bits = 0;
            for (char c : s) { int d = dec(c); if (d < 0) continue; val = (val << 6) | d; bits += 6; if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((val >> bits) & 0xFF)); } }
            return in.str(out); }, 1);
    }

    // ---- structuredClone (deep clone of objects/arrays/Map/Set/primitives) ----
    def("structuredClone", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        return in.deep_clone(arg(a, 0)); }, 1);

    // ---- JSON ----
    {
        JSObject* json = new_object();
        json->set(u"stringify", Value::make_heap_ptr(new_native(u"stringify", [](Interpreter& in, Value, std::vector<Value>& a) {
            return in.str(narrow(in.json_stringify(arg(a, 0)))); })));
        json->set(u"parse", Value::make_heap_ptr(new_native(u"parse", [](Interpreter& in, Value, std::vector<Value>& a) {
            std::u16string src = in.to_string(arg(a, 0));
            JsonParser p{in, src};
            return p.value();  // (malformed input yields a best-effort/undefined value)
        })));
        global_->define(u"JSON", Value::make_heap_ptr(json));
    }

    // ---- Object.prototype ----
    method(object_proto_, "hasOwnProperty", [](Interpreter& in, Value t, std::vector<Value>& a) {
        if (t.is_null() || t.is_undefined())
            in.throw_error(
                u"TypeError",
                u"Cannot convert undefined or null to object");
        return Value::make_bool(
            has_own_property(t, in.to_property_key(arg(a, 0))));
    }, 1);
    method(object_proto_, "isPrototypeOf", [](Interpreter&, Value t, std::vector<Value>& a) {
        Value v = arg(a, 0);
        JSObject* vo = as_proto_object(v);
        if (!t.is_heap_ptr() || !vo) return Value::make_bool(false);
        HeapObject* th = t.as_heap_ptr();
        for (JSObject* p = vo->proto; p; p = p->proto)
            if (p == th) return Value::make_bool(true);
        return Value::make_bool(false); }, 1);
    method(object_proto_, "propertyIsEnumerable", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string key = in.to_property_key(arg(a, 0));
        if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSArray) {
            auto* array = static_cast<JSArray*>(t.as_heap_ptr());
            size_t index;
            if (parse_index_u16(key, index))
                return Value::make_bool(array->has_index(index));
        }
        if (t.is_heap_ptr() && (t.as_heap_ptr()->kind == HeapObject::kJSObject || t.as_heap_ptr()->kind == HeapObject::kJSArray)) {
            const Property* p =
                static_cast<JSObject*>(t.as_heap_ptr())->find_own(key);
            return Value::make_bool(p && p->enumerable);
        }
        if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSFunction) {
            const Property* p =
                static_cast<JSFunction*>(t.as_heap_ptr())->find_own(key);
            return Value::make_bool(p && p->enumerable);
        }
        return Value::make_bool(false); }, 1);
    method(object_proto_, "valueOf", [](Interpreter&, Value t, std::vector<Value>&) { return t; });
    auto obj_to_string = [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
        if (t.is_undefined()) return in.str(std::u16string(u"[object Undefined]"));
        if (t.is_null())      return in.str(std::u16string(u"[object Null]"));
        const char16_t* tag = u"Object";
        if (t.is_heap_ptr()) {
            switch (t.as_heap_ptr()->kind) {
                case HeapObject::kJSObject:
                    if (static_cast<JSObject*>(t.as_heap_ptr())->has_own(u"%isRegExp%"))
                        tag = u"RegExp";
                    break;
                case HeapObject::kJSArray:    tag = u"Array"; break;
                case HeapObject::kJSFunction: tag = u"Function"; break;
                case HeapObject::kJSString:   tag = u"String"; break;
                case HeapObject::kJSBigInt:   tag = u"BigInt"; break;
                case HeapObject::kJSPromise:  tag = u"Promise"; break;
                case HeapObject::kJSMap:      tag = u"Map"; break;
                case HeapObject::kJSSet:      tag = u"Set"; break;
                case HeapObject::kArrayBuffer: tag = u"ArrayBuffer"; break;
                case HeapObject::kDataView:   tag = u"DataView"; break;
                case HeapObject::kTypedArray: {
                    switch (static_cast<JSTypedArray*>(
                                t.as_heap_ptr())->ta_kind) {
                        case TAKind::Int8: tag = u"Int8Array"; break;
                        case TAKind::Uint8: tag = u"Uint8Array"; break;
                        case TAKind::Uint8Clamped:
                            tag = u"Uint8ClampedArray";
                            break;
                        case TAKind::Int16: tag = u"Int16Array"; break;
                        case TAKind::Uint16: tag = u"Uint16Array"; break;
                        case TAKind::Int32: tag = u"Int32Array"; break;
                        case TAKind::Uint32: tag = u"Uint32Array"; break;
                        case TAKind::Float32: tag = u"Float32Array"; break;
                        case TAKind::Float64: tag = u"Float64Array"; break;
                    }
                    break;
                }
                default: break;
            }
        } else if (t.is_double() || t.is_int32()) tag = u"Number";
        else if (t.is_bool()) tag = u"Boolean";
        return in.str(u"[object " + std::u16string(tag) + u"]");
    };
    method(object_proto_, "toString", obj_to_string);
    method(object_proto_, "toLocaleString", [](Interpreter& in, Value t, std::vector<Value>&) {
        return in.str(narrow(in.to_string(t))); });

    // ---- Promise constructor + statics ----
    {
        JSFunction* Promise = new_native(u"Promise", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSPromise* p = in.new_promise();
            Value pv = Value::make_heap_ptr(p);
            Value executor = arg(a, 0);
            if (in.is_callable(executor)) {
                JSFunction* resolve = in.new_native(u"resolve", nullptr);
                resolve->set(u"%promise%", pv, false);
                resolve->native = [resolve](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        resolve->get(u"%promise%").as_heap_ptr());
                    in2.resolve_promise(
                        promise, ra.empty() ? Value::make_undefined() : ra[0]);
                    return Value::make_undefined();
                };
                JSFunction* reject = in.new_native(u"reject", nullptr);
                reject->set(u"%promise%", pv, false);
                reject->native = [reject](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        reject->get(u"%promise%").as_heap_ptr());
                    in2.reject_promise(
                        promise, ra.empty() ? Value::make_undefined() : ra[0]);
                    return Value::make_undefined();
                };
                in.push_root(pv);
                try {
                    std::vector<Value> args{Value::make_heap_ptr(resolve), Value::make_heap_ptr(reject)};
                    in.call(executor, Value::make_undefined(), args);
                } catch (ThrowSignal& sig) { in.reject_promise(p, sig.value); }
                in.pop_root();
            }
            return pv;
        });
        Promise->set(u"resolve", Value::make_heap_ptr(new_native(u"resolve", [](Interpreter& in, Value, std::vector<Value>& a) {
            return Value::make_heap_ptr(in.promise_resolve_value(arg(a, 0)));
        })));
        Promise->set(u"reject", Value::make_heap_ptr(new_native(u"reject", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSPromise* p = in.new_promise(); in.reject_promise(p, arg(a, 0)); return Value::make_heap_ptr(p);
        })));
        Promise->set(u"all", Value::make_heap_ptr(new_native(u"all", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSPromise* result = in.new_promise();
            Value rv = Value::make_heap_ptr(result);
            Value iter = arg(a, 0);
            if (!iter.is_heap_ptr() || iter.as_heap_ptr()->kind != HeapObject::kJSArray) { in.resolve_promise(result, Value::make_heap_ptr(in.new_array())); return rv; }
            auto* arr = static_cast<JSArray*>(iter.as_heap_ptr());
            auto* out = in.new_array();
            out->elements.resize(arr->elements.size(), Value::make_undefined());
            auto remaining = std::make_shared<int>(static_cast<int>(arr->elements.size()));
            if (*remaining == 0) { in.resolve_promise(result, Value::make_heap_ptr(out)); return rv; }
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                JSPromise* ep = in.promise_resolve_value(arr->elements[i]);
                JSFunction* onF = in.new_native(u"", nullptr);
                onF->set(u"%result%", rv, false);
                onF->set(u"%output%", Value::make_heap_ptr(out), false);
                onF->native = [onF, remaining, i](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        onF->get(u"%result%").as_heap_ptr());
                    auto* output = static_cast<JSArray*>(
                        onF->get(u"%output%").as_heap_ptr());
                    output->elements[i] = ra.empty() ? Value::make_undefined() : ra[0];
                    if (--*remaining == 0)
                        in2.resolve_promise(promise, Value::make_heap_ptr(output));
                    return Value::make_undefined();
                };
                JSFunction* onR = in.new_native(u"", nullptr);
                onR->set(u"%result%", rv, false);
                onR->native = [onR](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        onR->get(u"%result%").as_heap_ptr());
                    in2.reject_promise(
                        promise, ra.empty() ? Value::make_undefined() : ra[0]);
                    return Value::make_undefined();
                };
                in.promise_then(ep, Value::make_heap_ptr(onF), Value::make_heap_ptr(onR));
            }
            return rv;
        })));
        Promise->set(u"race", Value::make_heap_ptr(new_native(u"race", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSPromise* result = in.new_promise();
            Value iter = arg(a, 0);
            if (iter.is_heap_ptr() && iter.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* arr = static_cast<JSArray*>(iter.as_heap_ptr());
                for (auto& el : arr->elements) {
                    JSPromise* ep = in.promise_resolve_value(el);
                    JSFunction* onF = in.new_native(u"", nullptr);
                    onF->set(u"%result%", Value::make_heap_ptr(result), false);
                    onF->native = [onF](Interpreter& in2, Value, std::vector<Value>& ra) {
                        auto* promise = static_cast<JSPromise*>(
                            onF->get(u"%result%").as_heap_ptr());
                        in2.resolve_promise(
                            promise, ra.empty() ? Value::make_undefined() : ra[0]);
                        return Value::make_undefined();
                    };
                    JSFunction* onR = in.new_native(u"", nullptr);
                    onR->set(u"%result%", Value::make_heap_ptr(result), false);
                    onR->native = [onR](Interpreter& in2, Value, std::vector<Value>& ra) {
                        auto* promise = static_cast<JSPromise*>(
                            onR->get(u"%result%").as_heap_ptr());
                        in2.reject_promise(
                            promise, ra.empty() ? Value::make_undefined() : ra[0]);
                        return Value::make_undefined();
                    };
                    in.promise_then(ep, Value::make_heap_ptr(onF), Value::make_heap_ptr(onR));
                }
            }
            return Value::make_heap_ptr(result);
        })));
        Promise->set(u"allSettled", Value::make_heap_ptr(new_native(u"allSettled", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSPromise* result = in.new_promise(); Value rv = Value::make_heap_ptr(result);
            std::vector<Value> items = in.to_values(arg(a, 0));
            auto* out = in.new_array(); out->elements.resize(items.size(), Value::make_undefined());
            auto remaining = std::make_shared<int>(static_cast<int>(items.size()));
            if (*remaining == 0) { in.resolve_promise(result, Value::make_heap_ptr(out)); return rv; }
            for (size_t i = 0; i < items.size(); ++i) {
                JSPromise* ep = in.promise_resolve_value(items[i]);
                JSFunction* onF = in.new_native(u"", nullptr);
                onF->set(u"%result%", rv, false);
                onF->set(u"%output%", Value::make_heap_ptr(out), false);
                onF->native = [onF, remaining, i](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        onF->get(u"%result%").as_heap_ptr());
                    auto* output = static_cast<JSArray*>(
                        onF->get(u"%output%").as_heap_ptr());
                    JSObject* o = in2.new_object(); o->set(u"status", in2.str(std::string("fulfilled"))); o->set(u"value", ra.empty() ? Value::make_undefined() : ra[0]);
                    output->elements[i] = Value::make_heap_ptr(o);
                    if (--*remaining == 0)
                        in2.resolve_promise(promise, Value::make_heap_ptr(output));
                    return Value::make_undefined();
                };
                JSFunction* onR = in.new_native(u"", nullptr);
                onR->set(u"%result%", rv, false);
                onR->set(u"%output%", Value::make_heap_ptr(out), false);
                onR->native = [onR, remaining, i](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        onR->get(u"%result%").as_heap_ptr());
                    auto* output = static_cast<JSArray*>(
                        onR->get(u"%output%").as_heap_ptr());
                    JSObject* o = in2.new_object(); o->set(u"status", in2.str(std::string("rejected"))); o->set(u"reason", ra.empty() ? Value::make_undefined() : ra[0]);
                    output->elements[i] = Value::make_heap_ptr(o);
                    if (--*remaining == 0)
                        in2.resolve_promise(promise, Value::make_heap_ptr(output));
                    return Value::make_undefined();
                };
                in.promise_then(ep, Value::make_heap_ptr(onF), Value::make_heap_ptr(onR));
            }
            return rv;
        })));
        Promise->set(u"any", Value::make_heap_ptr(new_native(u"any", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSPromise* result = in.new_promise(); Value rv = Value::make_heap_ptr(result);
            std::vector<Value> items = in.to_values(arg(a, 0));
            auto remaining = std::make_shared<int>(static_cast<int>(items.size()));
            if (*remaining == 0) { in.reject_promise(result, in.str(std::string("AggregateError: All promises were rejected"))); return rv; }
            for (auto& it : items) {
                JSPromise* ep = in.promise_resolve_value(it);
                JSFunction* onF = in.new_native(u"", nullptr);
                onF->set(u"%result%", rv, false);
                onF->native = [onF](Interpreter& in2, Value, std::vector<Value>& ra) {
                    auto* promise = static_cast<JSPromise*>(
                        onF->get(u"%result%").as_heap_ptr());
                    in2.resolve_promise(
                        promise, ra.empty() ? Value::make_undefined() : ra[0]);
                    return Value::make_undefined();
                };
                JSFunction* onR = in.new_native(u"", nullptr);
                onR->set(u"%result%", rv, false);
                onR->native = [onR, remaining](Interpreter& in2, Value, std::vector<Value>&) {
                    auto* promise = static_cast<JSPromise*>(
                        onR->get(u"%result%").as_heap_ptr());
                    if (--*remaining == 0)
                        in2.reject_promise(
                            promise,
                            in2.str(std::string(
                                "AggregateError: All promises were rejected")));
                    return Value::make_undefined();
                };
                in.promise_then(ep, Value::make_heap_ptr(onF), Value::make_heap_ptr(onR));
            }
            return rv;
        })));
        global_->define(u"Promise", Value::make_heap_ptr(Promise));
    }

    // ---- Date (UTC-backed, millisecond precision) ----
    {
        JSObject* dproto = new_object();
        auto getms = [](Interpreter& in, Value t) -> double {
            if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSObject) {
                Value v = static_cast<JSObject*>(t.as_heap_ptr())->get(u"%ms%");
                if (!v.is_undefined())
                    return v.is_double() ? v.as_double()
                                         : (v.is_int32() ? v.as_int32()
                                                         : in.to_number(v));
            }
            in.throw_error(u"TypeError",
                           u"Date method called on an incompatible receiver");
        };
        auto setms = [](Value t, double milliseconds) {
            static_cast<JSObject*>(t.as_heap_ptr())
                ->set(u"%ms%", Value::make_double(time_clip(milliseconds)), false);
        };
        auto get_part = [getms](Interpreter& in, Value t, int part) -> Value {
            std::tm tmv{};
            int milliseconds = 0;
            if (!split_date(getms(in, t), tmv, milliseconds))
                return Value::make_double(std::numeric_limits<double>::quiet_NaN());
            int result = 0;
            switch (part) {
                case 0: result = tmv.tm_year + 1900; break;
                case 1: result = tmv.tm_mon; break;
                case 2: result = tmv.tm_mday; break;
                case 3: result = tmv.tm_wday; break;
                case 4: result = tmv.tm_hour; break;
                case 5: result = tmv.tm_min; break;
                case 6: result = tmv.tm_sec; break;
                default: result = milliseconds; break;
            }
            return Value::make_int32(result);
        };
        auto define_getter = [&](const char16_t* name, int part) {
            dproto->set(name, Value::make_heap_ptr(new_native(
                name, [get_part, part](Interpreter& in, Value t,
                                      std::vector<Value>&) {
                    return get_part(in, t, part);
                })));
        };
        define_getter(u"getFullYear", 0);
        define_getter(u"getUTCFullYear", 0);
        define_getter(u"getMonth", 1);
        define_getter(u"getUTCMonth", 1);
        define_getter(u"getDate", 2);
        define_getter(u"getUTCDate", 2);
        define_getter(u"getDay", 3);
        define_getter(u"getUTCDay", 3);
        define_getter(u"getHours", 4);
        define_getter(u"getUTCHours", 4);
        define_getter(u"getMinutes", 5);
        define_getter(u"getUTCMinutes", 5);
        define_getter(u"getSeconds", 6);
        define_getter(u"getUTCSeconds", 6);
        define_getter(u"getMilliseconds", 7);
        define_getter(u"getUTCMilliseconds", 7);

        dproto->set(u"getTime", Value::make_heap_ptr(new_native(
            u"getTime", [getms](Interpreter& in, Value t,
                                std::vector<Value>&) {
                return Value::make_double(getms(in, t));
            })));
        dproto->set(u"valueOf", dproto->get(u"getTime"));
        dproto->set(u"getTimezoneOffset", Value::make_heap_ptr(new_native(
            u"getTimezoneOffset", [getms](Interpreter& in, Value t,
                                          std::vector<Value>&) {
                double milliseconds = getms(in, t);
                return std::isfinite(milliseconds)
                           ? Value::make_int32(0)
                           : Value::make_double(
                                 std::numeric_limits<double>::quiet_NaN());
            })));
        dproto->set(u"getYear", Value::make_heap_ptr(new_native(
            u"getYear", [get_part](Interpreter& in, Value t,
                                   std::vector<Value>&) {
                Value year = get_part(in, t, 0);
                return year.is_int32()
                           ? Value::make_int32(year.as_int32() - 1900)
                           : year;
            })));

        enum DatePart { Millisecond, Second, Minute, Hour, Day, Month, Year };
        auto mutate = [getms, setms](Interpreter& in, Value t,
                                     std::vector<Value>& a,
                                     DatePart part) -> Value {
            double current = getms(in, t);
            if (part == Millisecond && !std::isfinite(current)) {
                setms(t, current);
                return Value::make_double(current);
            }
            if (!std::isfinite(current)) {
                if (part != Year) {
                    setms(t, std::numeric_limits<double>::quiet_NaN());
                    return Value::make_double(
                        std::numeric_limits<double>::quiet_NaN());
                }
                current = 0;
            }
            std::tm tmv{};
            int milliseconds = 0;
            split_date(current, tmv, milliseconds);
            auto number = [&in, &a](size_t index, double fallback) {
                return index < a.size() && !a[index].is_undefined()
                           ? in.to_number(a[index])
                           : fallback;
            };
            switch (part) {
                case Millisecond:
                    milliseconds = static_cast<int>(number(0, 0));
                    break;
                case Second:
                    tmv.tm_sec = static_cast<int>(number(0, 0));
                    milliseconds = static_cast<int>(number(1, milliseconds));
                    break;
                case Minute:
                    tmv.tm_min = static_cast<int>(number(0, 0));
                    tmv.tm_sec = static_cast<int>(number(1, tmv.tm_sec));
                    milliseconds = static_cast<int>(number(2, milliseconds));
                    break;
                case Hour:
                    tmv.tm_hour = static_cast<int>(number(0, 0));
                    tmv.tm_min = static_cast<int>(number(1, tmv.tm_min));
                    tmv.tm_sec = static_cast<int>(number(2, tmv.tm_sec));
                    milliseconds = static_cast<int>(number(3, milliseconds));
                    break;
                case Day:
                    tmv.tm_mday = static_cast<int>(number(0, 0));
                    break;
                case Month:
                    tmv.tm_mon = static_cast<int>(number(0, 0));
                    tmv.tm_mday = static_cast<int>(number(1, tmv.tm_mday));
                    break;
                case Year:
                    tmv.tm_year = static_cast<int>(number(0, 0)) - 1900;
                    tmv.tm_mon = static_cast<int>(number(1, tmv.tm_mon));
                    tmv.tm_mday = static_cast<int>(number(2, tmv.tm_mday));
                    break;
            }
            double result = join_date(tmv, milliseconds);
            setms(t, result);
            return Value::make_double(result);
        };
        auto define_setter = [&](const char16_t* name, DatePart part,
                                 uint32_t arity) {
            dproto->set(name, Value::make_heap_ptr(new_native(
                name, [mutate, part](Interpreter& in, Value t,
                                     std::vector<Value>& a) {
                    return mutate(in, t, a, part);
                }, arity)));
        };
        define_setter(u"setMilliseconds", Millisecond, 1);
        define_setter(u"setUTCMilliseconds", Millisecond, 1);
        define_setter(u"setSeconds", Second, 2);
        define_setter(u"setUTCSeconds", Second, 2);
        define_setter(u"setMinutes", Minute, 3);
        define_setter(u"setUTCMinutes", Minute, 3);
        define_setter(u"setHours", Hour, 4);
        define_setter(u"setUTCHours", Hour, 4);
        define_setter(u"setDate", Day, 1);
        define_setter(u"setUTCDate", Day, 1);
        define_setter(u"setMonth", Month, 2);
        define_setter(u"setUTCMonth", Month, 2);
        define_setter(u"setFullYear", Year, 3);
        define_setter(u"setUTCFullYear", Year, 3);
        dproto->set(u"setTime", Value::make_heap_ptr(new_native(
            u"setTime", [getms, setms](Interpreter& in, Value t,
                                       std::vector<Value>& a) {
                (void)getms(in, t);
                double result = time_clip(in.to_number(arg(a, 0)));
                setms(t, result);
                return Value::make_double(result);
            }, 1)));
        dproto->set(u"setYear", Value::make_heap_ptr(new_native(
            u"setYear", [mutate](Interpreter& in, Value t,
                                 std::vector<Value>& a) {
                double year = in.to_number(arg(a, 0));
                if (std::isfinite(year) && year >= 0 && year <= 99) {
                    std::vector<Value> adjusted{
                        Value::make_double(year + 1900)};
                    return mutate(in, t, adjusted, Year);
                }
                return mutate(in, t, a, Year);
            }, 1)));

        auto iso_string = [getms](Interpreter& in, Value t) -> Value {
            double ms = getms(in, t);
            std::tm tmv{};
            int milliseconds = 0;
            if (!split_date(ms, tmv, milliseconds))
                in.throw_error(u"RangeError", u"Invalid time value");
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec, milliseconds);
            return in.str(std::string(buf));
        };
        dproto->set(u"toISOString", Value::make_heap_ptr(new_native(
            u"toISOString", [iso_string](Interpreter& in, Value t,
                                         std::vector<Value>&) {
                return iso_string(in, t);
            })));
        auto readable_string = [getms](Interpreter& in, Value t) -> Value {
            double ms = getms(in, t);
            std::tm tmv{};
            int milliseconds = 0;
            if (!split_date(ms, tmv, milliseconds))
                return in.str("Invalid Date");
            char buf[80];
            std::strftime(buf, sizeof(buf), "%a %b %d %Y %H:%M:%S GMT+0000",
                          &tmv);
            return in.str(std::string(buf));
        };
        dproto->set(u"toString", Value::make_heap_ptr(new_native(
            u"toString", [readable_string](Interpreter& in, Value t,
                                           std::vector<Value>&) {
                return readable_string(in, t);
            })));
        dproto->set(u"toUTCString", Value::make_heap_ptr(new_native(
            u"toUTCString", [getms](Interpreter& in, Value t,
                                     std::vector<Value>&) {
                double ms = getms(in, t);
                std::tm tmv{};
                int milliseconds = 0;
                if (!split_date(ms, tmv, milliseconds))
                    return in.str("Invalid Date");
                char buf[64];
                std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT",
                              &tmv);
                return in.str(std::string(buf));
            })));
        dproto->set(u"toGMTString", dproto->get(u"toUTCString"));
        dproto->set(u"toLocaleString", dproto->get(u"toString"));
        dproto->set(u"toLocaleDateString", dproto->get(u"toString"));
        dproto->set(u"toLocaleTimeString", dproto->get(u"toString"));
        dproto->set(u"toDateString", dproto->get(u"toString"));
        dproto->set(u"toTimeString", dproto->get(u"toString"));
        dproto->set(u"toJSON", Value::make_heap_ptr(new_native(
            u"toJSON", [getms, iso_string](Interpreter& in, Value t,
                                           std::vector<Value>&) {
                if (!std::isfinite(getms(in, t))) return Value::make_null();
                return iso_string(in, t);
            }, 1)));

        JSFunction* Date = new_native(
            u"Date", [dproto, readable_string](Interpreter& in, Value this_value,
                                               std::vector<Value>& a) -> Value {
            bool constructing =
                this_value.is_heap_ptr() &&
                this_value.as_heap_ptr()->kind == HeapObject::kJSObject &&
                static_cast<JSObject*>(this_value.as_heap_ptr())->proto == dproto;
            if (!constructing)
                return readable_string(in, [&]() {
                    JSObject* now = in.new_object();
                    now->proto = dproto;
                    now->set(
                        u"%ms%",
                        Value::make_double(static_cast<double>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count())),
                        false);
                    return Value::make_heap_ptr(now);
                }());
            double ms;
            if (a.empty()) {
                ms = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
            } else if (a.size() == 1) {
                if (a[0].is_heap_ptr() &&
                    a[0].as_heap_ptr()->kind == HeapObject::kJSString)
                    ms = parse_date_string(
                        narrow(static_cast<JSString*>(a[0].as_heap_ptr())->data));
                else if (a[0].is_heap_ptr() &&
                         a[0].as_heap_ptr()->kind == HeapObject::kJSObject &&
                         !static_cast<JSObject*>(a[0].as_heap_ptr())
                              ->get(u"%ms%")
                              .is_undefined())
                    ms = in.to_number(
                        static_cast<JSObject*>(a[0].as_heap_ptr())->get(u"%ms%"));
                else
                    ms = in.to_number(a[0]);
            } else {
                std::tm tmv{};
                int year = in.to_int32(a[0]);
                if (year >= 0 && year <= 99) year += 1900;
                tmv.tm_year = year - 1900;
                tmv.tm_mon = in.to_int32(arg(a, 1));
                tmv.tm_mday = a.size() > 2 ? in.to_int32(a[2]) : 1;
                tmv.tm_hour = a.size() > 3 ? in.to_int32(a[3]) : 0;
                tmv.tm_min = a.size() > 4 ? in.to_int32(a[4]) : 0;
                tmv.tm_sec = a.size() > 5 ? in.to_int32(a[5]) : 0;
                int milliseconds = a.size() > 6 ? in.to_int32(a[6]) : 0;
                ms = join_date(tmv, milliseconds);
            }
            ms = time_clip(ms);
            JSObject* d = in.new_object(); d->proto = dproto; d->set(u"%ms%", Value::make_double(ms), false);
            return Value::make_heap_ptr(d);
        }, 7);
        Date->set(u"now", Value::make_heap_ptr(new_native(
            u"now", [](Interpreter&, Value, std::vector<Value>&) {
                return Value::make_double(static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()));
            })));
        Date->set(u"parse", Value::make_heap_ptr(new_native(
            u"parse", [](Interpreter& in, Value, std::vector<Value>& a) {
                return Value::make_double(
                    parse_date_string(narrow(in.to_string(arg(a, 0)))));
            }, 1)));
        Date->set(u"UTC", Value::make_heap_ptr(new_native(
            u"UTC", [](Interpreter& in, Value, std::vector<Value>& a) {
                int year = in.to_int32(arg(a, 0));
                if (year >= 0 && year <= 99) year += 1900;
                std::tm tmv{};
                tmv.tm_year = year - 1900;
                tmv.tm_mon = a.size() > 1 ? in.to_int32(a[1]) : 0;
                tmv.tm_mday = a.size() > 2 ? in.to_int32(a[2]) : 1;
                tmv.tm_hour = a.size() > 3 ? in.to_int32(a[3]) : 0;
                tmv.tm_min = a.size() > 4 ? in.to_int32(a[4]) : 0;
                tmv.tm_sec = a.size() > 5 ? in.to_int32(a[5]) : 0;
                int milliseconds = a.size() > 6 ? in.to_int32(a[6]) : 0;
                return Value::make_double(join_date(tmv, milliseconds));
            }, 7)));
        dproto->set(u"constructor", Value::make_heap_ptr(Date), false);
        Date->set(u"prototype", Value::make_heap_ptr(dproto), false);
        global_->define(u"Date", Value::make_heap_ptr(Date));
    }

    // ---- Performance timeline (monotonic clock + marks/measures) ----
    {
        const auto steady_origin = std::chrono::steady_clock::now();
        const double time_origin = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto now = [steady_origin]() {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - steady_origin).count();
        };
        JSObject* perf = new_object();
        JSArray* entries = new_array();
        perf->set(u"%entries%", Value::make_heap_ptr(entries), false);
        perf->set(u"timeOrigin", Value::make_double(time_origin), false);
        perf->set(u"now", Value::make_heap_ptr(new_native(u"now",
            [now](Interpreter&, Value, std::vector<Value>&) {
                return Value::make_double(now());
            })));
        perf->set(u"mark", Value::make_heap_ptr(new_native(u"mark",
            [entries, now](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                JSObject* entry = in.new_object();
                entry->set(u"name", in.str(narrow(in.to_string(arg(a, 0)))));
                entry->set(u"entryType", in.str("mark"));
                double start_time = now();
                Value detail = Value::make_null();
                if (a.size() > 1 && a[1].is_heap_ptr()) {
                    Value requested = in.get_prop_public(a[1], u"startTime");
                    if (!requested.is_undefined()) {
                        start_time = in.to_number(requested);
                        if (!std::isfinite(start_time) || start_time < 0)
                            in.throw_error(u"TypeError", u"PerformanceMark startTime must be non-negative");
                    }
                    Value requested_detail = in.get_prop_public(a[1], u"detail");
                    if (!requested_detail.is_undefined()) detail = requested_detail;
                }
                entry->set(u"startTime", Value::make_double(start_time));
                entry->set(u"duration", Value::make_double(0));
                entry->set(u"detail", detail);
                entries->elements.push_back(Value::make_heap_ptr(entry));
                return Value::make_heap_ptr(entry);
            }, 1)));
        auto matching_entries = [entries](Interpreter& in,
                                           const std::u16string* name,
                                           const std::u16string* type) {
            JSArray* out = in.new_array();
            for (Value value : entries->elements) {
                if (!value.is_heap_ptr() || value.as_heap_ptr()->kind != HeapObject::kJSObject)
                    continue;
                auto* entry = static_cast<JSObject*>(value.as_heap_ptr());
                if (name && in.to_string(entry->get(u"name")) != *name) continue;
                if (type && in.to_string(entry->get(u"entryType")) != *type) continue;
                out->elements.push_back(value);
            }
            return Value::make_heap_ptr(out);
        };
        perf->set(u"getEntries", Value::make_heap_ptr(new_native(u"getEntries",
            [matching_entries](Interpreter& in, Value, std::vector<Value>&) {
                return matching_entries(in, nullptr, nullptr);
            })));
        perf->set(u"getEntriesByName", Value::make_heap_ptr(new_native(u"getEntriesByName",
            [matching_entries](Interpreter& in, Value, std::vector<Value>& a) {
                std::u16string name = in.to_string(arg(a, 0));
                if (a.size() > 1 && !a[1].is_undefined()) {
                    std::u16string type = in.to_string(a[1]);
                    return matching_entries(in, &name, &type);
                }
                return matching_entries(in, &name, nullptr);
            }, 1)));
        perf->set(u"getEntriesByType", Value::make_heap_ptr(new_native(u"getEntriesByType",
            [matching_entries](Interpreter& in, Value, std::vector<Value>& a) {
                std::u16string type = in.to_string(arg(a, 0));
                return matching_entries(in, nullptr, &type);
            }, 1)));
        perf->set(u"clearMarks", Value::make_heap_ptr(new_native(u"clearMarks",
            [entries](Interpreter& in, Value, std::vector<Value>& a) {
                if (a.empty() || a[0].is_undefined()) {
                    entries->elements.erase(
                        std::remove_if(entries->elements.begin(), entries->elements.end(),
                            [&in](Value value) {
                                if (!value.is_heap_ptr() ||
                                    value.as_heap_ptr()->kind != HeapObject::kJSObject)
                                    return false;
                                return in.to_string(
                                    static_cast<JSObject*>(value.as_heap_ptr())
                                        ->get(u"entryType")) == u"mark";
                            }),
                        entries->elements.end());
                    return Value::make_undefined();
                }
                std::u16string name = in.to_string(a[0]);
                entries->elements.erase(
                    std::remove_if(entries->elements.begin(), entries->elements.end(),
                        [&in, &name](Value value) {
                            if (!value.is_heap_ptr() ||
                                value.as_heap_ptr()->kind != HeapObject::kJSObject)
                                return false;
                            auto* entry = static_cast<JSObject*>(value.as_heap_ptr());
                            return in.to_string(entry->get(u"entryType")) == u"mark" &&
                                   in.to_string(entry->get(u"name")) == name;
                        }),
                    entries->elements.end());
                return Value::make_undefined();
            })));
        perf->set(u"measure", Value::make_heap_ptr(new_native(u"measure",
            [entries, now](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                auto mark_time = [entries, &in](const std::u16string& name) -> double {
                    for (size_t i = entries->elements.size(); i-- > 0;) {
                        Value value = entries->elements[i];
                        if (!value.is_heap_ptr() ||
                            value.as_heap_ptr()->kind != HeapObject::kJSObject)
                            continue;
                        auto* entry = static_cast<JSObject*>(value.as_heap_ptr());
                        if (in.to_string(entry->get(u"entryType")) == u"mark" &&
                            in.to_string(entry->get(u"name")) == name)
                            return in.to_number(entry->get(u"startTime"));
                    }
                    in.throw_error(u"SyntaxError", u"The mark does not exist");
                };
                double start = 0;
                double end = now();
                if (a.size() > 1 && !a[1].is_undefined())
                    start = mark_time(in.to_string(a[1]));
                if (a.size() > 2 && !a[2].is_undefined())
                    end = mark_time(in.to_string(a[2]));
                JSObject* entry = in.new_object();
                entry->set(u"name", in.str(narrow(in.to_string(arg(a, 0)))));
                entry->set(u"entryType", in.str("measure"));
                entry->set(u"startTime", Value::make_double(start));
                entry->set(u"duration", Value::make_double(end - start));
                entry->set(u"detail", Value::make_null());
                entries->elements.push_back(Value::make_heap_ptr(entry));
                return Value::make_heap_ptr(entry);
            }, 1)));
        perf->set(u"clearMeasures", Value::make_heap_ptr(new_native(u"clearMeasures",
            [entries](Interpreter& in, Value, std::vector<Value>& a) {
                bool all = a.empty() || a[0].is_undefined();
                std::u16string name = all ? std::u16string() : in.to_string(a[0]);
                entries->elements.erase(
                    std::remove_if(entries->elements.begin(), entries->elements.end(),
                        [&in, all, &name](Value value) {
                            if (!value.is_heap_ptr() ||
                                value.as_heap_ptr()->kind != HeapObject::kJSObject)
                                return false;
                            auto* entry = static_cast<JSObject*>(value.as_heap_ptr());
                            return in.to_string(entry->get(u"entryType")) == u"measure" &&
                                   (all || in.to_string(entry->get(u"name")) == name);
                        }),
                    entries->elements.end());
                return Value::make_undefined();
            })));
        global_->define(u"performance", Value::make_heap_ptr(perf));
    }

    // ---- crypto (randomUUID + getRandomValues over a length-bearing array) ----
    {
        JSObject* crypto = new_object();
        crypto->set(u"randomUUID", Value::make_heap_ptr(new_native(u"randomUUID", [](Interpreter& in, Value, std::vector<Value>&) {
            static const char* h = "0123456789abcdef";
            std::string u; u.reserve(36);
            for (int i = 0; i < 36; ++i) {
                if (i == 8 || i == 13 || i == 18 || i == 23) { u.push_back('-'); continue; }
                int r = std::rand() & 15;
                if (i == 14) r = 4;
                if (i == 19) r = (r & 0x3) | 0x8;
                u.push_back(h[r]);
            }
            return in.str(u); })));
        crypto->set(u"getRandomValues", Value::make_heap_ptr(new_native(u"getRandomValues", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value v = arg(a, 0);
            if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* arr = static_cast<JSArray*>(v.as_heap_ptr());
                for (auto& e : arr->elements) e = Value::make_int32(std::rand() & 0xFF);
            }
            (void)in; return v; })));
        global_->define(u"crypto", Value::make_heap_ptr(crypto));
    }

    // ---- TextEncoder / TextDecoder ----
    {
        JSFunction* TE = new_native(u"TextEncoder", [](Interpreter& in, Value, std::vector<Value>&) -> Value {
            JSObject* o = in.new_object();
            o->set(u"encode", Value::make_heap_ptr(in.new_native(u"encode", [](Interpreter& in2, Value, std::vector<Value>& a) {
                std::string encoded =
                    encode_utf8(in2.to_string(arg(a, 0)));
                JSArrayBuffer* buffer = in2.new_array_buffer(
                    std::vector<uint8_t>(encoded.begin(), encoded.end()));
                Value* ctor = in2.global()->find(u"Uint8Array");
                if (!ctor) return Value::make_heap_ptr(buffer);
                std::vector<Value> args{Value::make_heap_ptr(buffer)};
                return in2.construct(*ctor, args); })));
            o->set(u"encodeInto", Value::make_heap_ptr(in.new_native(
                u"encodeInto",
                [](Interpreter& in2, Value,
                   std::vector<Value>& a) -> Value {
                    std::string encoded =
                        encode_utf8(in2.to_string(arg(a, 0)));
                    Value target = arg(a, 1);
                    size_t written = 0;
                    if (target.is_heap_ptr() &&
                        target.as_heap_ptr()->kind ==
                            HeapObject::kTypedArray) {
                        auto* typed = static_cast<JSTypedArray*>(
                            target.as_heap_ptr());
                        written =
                            std::min(encoded.size(), typed->length);
                        for (size_t i = 0; i < written; ++i)
                            in2.ta_set_index(
                                typed, i,
                                Value::make_int32(
                                    static_cast<uint8_t>(
                                        encoded[i])));
                    }
                    JSObject* result = in2.new_object();
                    result->set(
                        u"read",
                        Value::make_int32(static_cast<int32_t>(
                            in2.to_string(arg(a, 0)).size())));
                    result->set(
                        u"written",
                        Value::make_int32(
                            static_cast<int32_t>(written)));
                    return Value::make_heap_ptr(result);
                },
                2)));
            return Value::make_heap_ptr(o); });
        global_->define(u"TextEncoder", Value::make_heap_ptr(TE));
        JSFunction* TD = new_native(u"TextDecoder", [](Interpreter& in, Value, std::vector<Value>&) -> Value {
            JSObject* o = in.new_object();
            o->set(u"decode", Value::make_heap_ptr(in.new_native(u"decode", [](Interpreter& in2, Value, std::vector<Value>& a) {
                std::vector<uint8_t> bytes;
                Value v = arg(a, 0);
                if (v.is_heap_ptr() &&
                    v.as_heap_ptr()->kind == HeapObject::kJSArray) {
                    for (Value e :
                         static_cast<JSArray*>(v.as_heap_ptr())
                             ->elements)
                        bytes.push_back(static_cast<uint8_t>(
                            in2.to_int32(e)));
                } else if (v.is_heap_ptr() &&
                           v.as_heap_ptr()->kind ==
                               HeapObject::kArrayBuffer) {
                    bytes = static_cast<JSArrayBuffer*>(
                                v.as_heap_ptr())
                                ->data;
                } else if (v.is_heap_ptr() &&
                           v.as_heap_ptr()->kind ==
                               HeapObject::kTypedArray) {
                    auto* typed = static_cast<JSTypedArray*>(
                        v.as_heap_ptr());
                    if (typed->buffer)
                        bytes.assign(
                            typed->buffer->data.begin() +
                                typed->byte_offset,
                            typed->buffer->data.begin() +
                                typed->byte_offset +
                                typed->byte_length());
                }
                return in2.str(decode_utf8(bytes)); })));
            return Value::make_heap_ptr(o); });
        global_->define(u"TextDecoder", Value::make_heap_ptr(TD));
    }

    // ---- URLSearchParams ----
    {
        JSObject* usp_proto = new_object();
        auto pairs_for = [](Interpreter& in, Value receiver) -> JSArray* {
            Value value = in.get_prop_public(receiver, u"%pairs%");
            if (!value.is_heap_ptr() ||
                value.as_heap_ptr()->kind != HeapObject::kJSArray)
                in.throw_error(
                    u"TypeError",
                    u"URLSearchParams method called on incompatible receiver");
            return static_cast<JSArray*>(value.as_heap_ptr());
        };
        JSFunction* USP = new_native(u"URLSearchParams", [usp_proto](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            JSObject* o = in.new_object();
            o->proto = usp_proto;
            JSArray* pairs = in.new_array();  // [[k,v], ...] backing store
            std::u16string q = arg(a, 0).is_undefined() ? u"" : in.to_string(arg(a, 0));
            if (!q.empty() && q[0] == '?') q = q.substr(1);
            size_t pos = 0;
            while (pos < q.size()) {
                size_t amp = q.find('&', pos); if (amp == std::u16string::npos) amp = q.size();
                std::u16string kv = q.substr(pos, amp - pos);
                size_t eq = kv.find('=');
                std::u16string k = eq == std::u16string::npos ? kv : kv.substr(0, eq);
                std::u16string val = eq == std::u16string::npos ? u"" : kv.substr(eq + 1);
                JSArray* pair = in.new_array(); pair->elements = {in.str(narrow(k)), in.str(narrow(val))};
                pairs->elements.push_back(Value::make_heap_ptr(pair));
                pos = amp + 1;
            }
            o->set(u"%pairs%", Value::make_heap_ptr(pairs), false);
            return Value::make_heap_ptr(o); });
        USP->set(u"prototype", Value::make_heap_ptr(usp_proto), false);
        usp_proto->set(u"constructor", Value::make_heap_ptr(USP), false);
        auto method = [&](const char16_t* name, NativeFn fn,
                          uint32_t arity) {
            usp_proto->set(
                name,
                Value::make_heap_ptr(
                    new_native(name, std::move(fn), arity)),
                false);
        };
        method(u"get", [pairs_for](Interpreter& in, Value receiver,
                                   std::vector<Value>& a) -> Value {
            std::u16string key = in.to_string(arg(a, 0));
            for (Value value : pairs_for(in, receiver)->elements) {
                auto* pair = static_cast<JSArray*>(
                    value.as_heap_ptr());
                if (in.to_string(pair->elements[0]) == key)
                    return pair->elements[1];
            }
            return Value::make_null();
        }, 1);
        method(u"getAll", [pairs_for](Interpreter& in, Value receiver,
                                      std::vector<Value>& a) -> Value {
            std::u16string key = in.to_string(arg(a, 0));
            JSArray* values = in.new_array();
            for (Value value : pairs_for(in, receiver)->elements) {
                auto* pair = static_cast<JSArray*>(
                    value.as_heap_ptr());
                if (in.to_string(pair->elements[0]) == key)
                    values->elements.push_back(pair->elements[1]);
            }
            return Value::make_heap_ptr(values);
        }, 1);
        method(u"has", [pairs_for](Interpreter& in, Value receiver,
                                   std::vector<Value>& a) -> Value {
            std::u16string key = in.to_string(arg(a, 0));
            bool compare_value =
                a.size() > 1 && !a[1].is_undefined();
            std::u16string expected =
                compare_value ? in.to_string(a[1]) : u"";
            for (Value value : pairs_for(in, receiver)->elements) {
                auto* pair = static_cast<JSArray*>(
                    value.as_heap_ptr());
                if (in.to_string(pair->elements[0]) == key &&
                    (!compare_value ||
                     in.to_string(pair->elements[1]) == expected))
                    return Value::make_bool(true);
            }
            return Value::make_bool(false);
        }, 1);
        method(u"append", [pairs_for](Interpreter& in, Value receiver,
                                      std::vector<Value>& a) -> Value {
            JSArray* pair = in.new_array();
            pair->elements = {
                in.str(narrow(in.to_string(arg(a, 0)))),
                in.str(narrow(in.to_string(arg(a, 1))))};
            pairs_for(in, receiver)
                ->elements.push_back(Value::make_heap_ptr(pair));
            return Value::make_undefined();
        }, 2);
        method(u"delete", [pairs_for](Interpreter& in, Value receiver,
                                      std::vector<Value>& a) -> Value {
            std::u16string key = in.to_string(arg(a, 0));
            bool compare_value =
                a.size() > 1 && !a[1].is_undefined();
            std::u16string expected =
                compare_value ? in.to_string(a[1]) : u"";
            auto& pairs = pairs_for(in, receiver)->elements;
            pairs.erase(
                std::remove_if(
                    pairs.begin(), pairs.end(),
                    [&](Value value) {
                        auto* pair = static_cast<JSArray*>(
                            value.as_heap_ptr());
                        return in.to_string(pair->elements[0]) == key &&
                               (!compare_value ||
                                in.to_string(pair->elements[1]) ==
                                    expected);
                    }),
                pairs.end());
            return Value::make_undefined();
        }, 1);
        method(u"set", [pairs_for](Interpreter& in, Value receiver,
                                   std::vector<Value>& a) -> Value {
            std::u16string key = in.to_string(arg(a, 0));
            Value replacement =
                in.str(narrow(in.to_string(arg(a, 1))));
            auto& pairs = pairs_for(in, receiver)->elements;
            bool replaced = false;
            pairs.erase(
                std::remove_if(
                    pairs.begin(), pairs.end(),
                    [&](Value value) {
                        auto* pair = static_cast<JSArray*>(
                            value.as_heap_ptr());
                        if (in.to_string(pair->elements[0]) != key)
                            return false;
                        if (!replaced) {
                            pair->elements[1] = replacement;
                            replaced = true;
                            return false;
                        }
                        return true;
                    }),
                pairs.end());
            if (!replaced) {
                JSArray* pair = in.new_array();
                pair->elements = {
                    in.str(narrow(key)), replacement};
                pairs.push_back(Value::make_heap_ptr(pair));
            }
            return Value::make_undefined();
        }, 2);
        method(u"forEach", [pairs_for](Interpreter& in, Value receiver,
                                       std::vector<Value>& a) -> Value {
            Value callback = arg(a, 0);
            Value this_arg = arg(a, 1);
            if (!in.is_callable(callback))
                in.throw_error(u"TypeError", u"callback is not a function");
            std::vector<Value> snapshot =
                pairs_for(in, receiver)->elements;
            for (Value value : snapshot) {
                auto* pair = static_cast<JSArray*>(
                    value.as_heap_ptr());
                std::vector<Value> arguments{
                    pair->elements[1], pair->elements[0], receiver};
                in.call(callback, this_arg, arguments);
            }
            return Value::make_undefined();
        }, 1);
        method(u"toString", [pairs_for](Interpreter& in, Value receiver,
                                        std::vector<Value>&) -> Value {
            std::u16string output;
            for (Value value : pairs_for(in, receiver)->elements) {
                auto* pair = static_cast<JSArray*>(
                    value.as_heap_ptr());
                if (!output.empty()) output += u"&";
                output += in.to_string(pair->elements[0]) + u"=" +
                          in.to_string(pair->elements[1]);
            }
            return in.str(narrow(output));
        }, 0);
        global_->define(u"URLSearchParams", Value::make_heap_ptr(USP));
    }

    // ---- URL ----
    {
        JSFunction* URL = new_native(u"URL", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string href = in.to_string(arg(a, 0));
            if (a.size() > 1 && !arg(a, 1).is_undefined()) {  // resolve against a base (host part only)
                std::u16string base = in.to_string(a[1]);
                if (!href.empty() && href[0] == '/') {
                    size_t sch = base.find(u"://");
                    if (sch != std::u16string::npos) { size_t slash = base.find('/', sch + 3); href = (slash == std::u16string::npos ? base : base.substr(0, slash)) + href; }
                }
            }
            JSObject* o = in.new_object();
            std::u16string rest = href, protocol, host, pathname = u"/", search, hash;
            size_t sch = rest.find(u"://");
            if (sch != std::u16string::npos) { protocol = rest.substr(0, sch + 1); rest = rest.substr(sch + 3); }
            size_t h = rest.find('#'); if (h != std::u16string::npos) { hash = rest.substr(h); rest = rest.substr(0, h); }
            size_t q = rest.find('?'); if (q != std::u16string::npos) { search = rest.substr(q); rest = rest.substr(0, q); }
            size_t slash = rest.find('/');
            if (slash != std::u16string::npos) { host = rest.substr(0, slash); pathname = rest.substr(slash); }
            else host = rest;
            std::u16string hostname = host; std::u16string port;
            size_t colon = host.find(':'); if (colon != std::u16string::npos) { hostname = host.substr(0, colon); port = host.substr(colon + 1); }
            o->set(u"href", in.str(narrow(href)));
            o->set(u"protocol", in.str(narrow(protocol)));
            o->set(u"host", in.str(narrow(host)));
            o->set(u"hostname", in.str(narrow(hostname)));
            o->set(u"port", in.str(narrow(port)));
            o->set(u"pathname", in.str(narrow(pathname)));
            o->set(u"search", in.str(narrow(search)));
            o->set(u"hash", in.str(narrow(hash)));
            o->set(u"origin", in.str(narrow(protocol.empty() ? std::u16string() : protocol + u"//" + host)));
            std::vector<Value> spa{ in.str(narrow(search)) };
            if (Value* uspv = in.global()->find(u"URLSearchParams"))
                o->set(u"searchParams", in.construct(*uspv, spa));
            return Value::make_heap_ptr(o); });
        global_->define(u"URL", Value::make_heap_ptr(URL));
    }

    // ---- timers / microtask scheduling (wired to the event loop) ----
    def("queueMicrotask", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        Value cb = arg(a, 0);
        if (in.is_callable(cb)) {
            trace_host_callback("schedule", "microtask", cb);
            in.enqueue_microtask([&in, cb]() {
                trace_host_callback("run", "microtask", cb);
                std::vector<Value> args;
                try { in.call(cb, Value::make_undefined(), args); }
                catch (ThrowSignal&) {}
            }, {cb});
        }
        return Value::make_undefined();
    }, 1);
    def("setTimeout", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        auto* loop = in.event_loop();
        Value cb = arg(a, 0);
        if (!loop || !in.is_callable(cb)) return Value::make_int32(0);
        uint64_t delay = a.size() > 1 ? static_cast<uint64_t>(in.to_number(a[1])) : 0;
        in.add_host_root(cb);
        Interpreter* self = &in;
        trace_host_callback("schedule", "timeout", cb, delay);
        uint64_t id = loop->set_timeout([self, cb, delay]() {
            trace_host_callback("run", "timeout", cb, delay);
            self->remove_host_root(cb);
            std::vector<Value> args; try { self->call(cb, Value::make_undefined(), args); } catch (ThrowSignal&) {}
        }, delay);
        return Value::make_double(static_cast<double>(id));
    }, 2);
    def("setInterval", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        auto* loop = in.event_loop();
        Value cb = arg(a, 0);
        if (!loop || !in.is_callable(cb)) return Value::make_int32(0);
        uint64_t iv = a.size() > 1 ? static_cast<uint64_t>(in.to_number(a[1])) : 0;
        in.add_host_root(cb);
        Interpreter* self = &in;
        trace_host_callback("schedule", "interval", cb, iv);
        uint64_t id = loop->set_interval([self, cb, iv]() {
            trace_host_callback("run", "interval", cb, iv);
            std::vector<Value> args; try { self->call(cb, Value::make_undefined(), args); } catch (ThrowSignal&) {}
        }, iv);
        return Value::make_double(static_cast<double>(id));
    }, 2);
    auto clear_timer = [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        if (auto* loop = in.event_loop()) loop->clear_timer(static_cast<uint64_t>(in.to_number(arg(a, 0))));
        return Value::make_undefined();
    };
    def("clearTimeout", clear_timer, 1);
    def("clearInterval", clear_timer, 1);
    def("cancelAnimationFrame", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        if (auto* loop = in.event_loop())
            loop->cancel_animation_frame(
                static_cast<uint64_t>(in.to_number(arg(a, 0))));
        return Value::make_undefined();
    }, 1);
    def("requestAnimationFrame", [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        auto* loop = in.event_loop();
        Value cb = arg(a, 0);
        if (!loop || !in.is_callable(cb)) return Value::make_int32(0);
        in.add_host_root(cb);
        Interpreter* self = &in;
        trace_host_callback("schedule", "raf", cb);
        uint64_t id = loop->request_animation_frame([self, cb](double ts) {
            trace_host_callback("run", "raf", cb);
            self->remove_host_root(cb);
            std::vector<Value> args{Value::make_double(ts)};
            try { self->call(cb, Value::make_undefined(), args); } catch (ThrowSignal&) {}
        });
        return Value::make_double(static_cast<double>(id));
    }, 1);

    // ---- Number.prototype ----
    {
        auto nm = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
            number_proto_->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
        };
        nm("valueOf",  [](Interpreter& in, Value t, std::vector<Value>&) {
            return Value::make_double(number_this(in, t));
        });
        nm("toString", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            double d = number_this(in, t);
            int radix = a.empty() || a[0].is_undefined() ? 10 : in.to_int32(a[0]);
            if (radix == 10 || radix < 2 || radix > 36) return in.str(narrow(in.to_string(Value::make_double(d))));
            if (std::isnan(d)) return in.str(std::u16string(u"NaN"));
            bool neg = d < 0; uint64_t n = static_cast<uint64_t>(neg ? -d : d);
            static const char* D = "0123456789abcdefghijklmnopqrstuvwxyz";
            std::string s; if (n == 0) s = "0"; while (n) { s.insert(s.begin(), D[n % radix]); n /= radix; }
            if (neg) s.insert(s.begin(), '-');
            return in.str(s); }, 1);
        nm("toLocaleString", [](Interpreter& in, Value t, std::vector<Value>&) {
            return in.str(narrow(in.to_string(Value::make_double(number_this(in, t)))));
        });
        nm("toFixed", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            double d = number_this(in, t); int digits = a.empty() ? 0 : in.to_int32(a[0]);
            if (digits < 0) digits = 0;
            if (digits > 100) digits = 100;
            char buf[400]; std::snprintf(buf, sizeof buf, "%.*f", digits, d);
            return in.str(std::string(buf)); }, 1);
        nm("toPrecision", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            double d = number_this(in, t);
            if (a.empty() || a[0].is_undefined()) return in.str(narrow(in.to_string(Value::make_double(d))));
            int p = in.to_int32(a[0]); if (p < 1) p = 1;
            if (p > 100) p = 100;
            char buf[400]; std::snprintf(buf, sizeof buf, "%.*g", p, d);
            return in.str(std::string(buf)); }, 1);
    }
    // ---- BigInt.prototype ----
    {
        auto bm = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
            bigint_proto_->set(u16(name), Value::make_heap_ptr(
                new_native(u16(name), std::move(fn), arity)), false);
        };
        bm("valueOf", [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            return bigint_this(in, t);
        });
        bm("toString", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            Value bigint = bigint_this(in, t);
            int radix = a.empty() || a[0].is_undefined() ? 10 : in.to_int32(a[0]);
            if (radix < 2 || radix > 36)
                in.throw_error(u"RangeError", u"toString() radix argument must be between 2 and 36");
            return in.str(static_cast<JSBigInt*>(bigint.as_heap_ptr())->value.get_str(radix));
        }, 1);
        bm("toLocaleString", [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
            Value bigint = bigint_this(in, t);
            return in.str(static_cast<JSBigInt*>(bigint.as_heap_ptr())->value.get_str());
        });
    }
    // ---- Boolean.prototype ----
    {
        boolean_proto_->set(u"valueOf", Value::make_heap_ptr(new_native(u"valueOf",
            [](Interpreter& in, Value t, std::vector<Value>&) {
                return Value::make_bool(boolean_this(in, t));
            })), false);
        boolean_proto_->set(u"toString", Value::make_heap_ptr(new_native(u"toString",
            [](Interpreter& in, Value t, std::vector<Value>&) {
                return in.str(std::u16string(boolean_this(in, t) ? u"true" : u"false"));
            })), false);
    }
    // ---- Symbol.prototype ----
    {
        symbol_proto_->set(
            u"valueOf",
            Value::make_heap_ptr(new_native(
                u"valueOf",
                [](Interpreter& in, Value t,
                   std::vector<Value>&) -> Value {
                    if (t.is_heap_ptr() &&
                        t.as_heap_ptr()->kind == HeapObject::kJSSymbol)
                        return t;
                    Value primitive = wrapped_primitive(t);
                    if (primitive.is_heap_ptr() &&
                        primitive.as_heap_ptr()->kind ==
                            HeapObject::kJSSymbol)
                        return primitive;
                    in.throw_error(
                        u"TypeError",
                        u"Symbol.prototype.valueOf called on incompatible receiver");
                })),
            false);
        symbol_proto_->set(
            u"toString",
            Value::make_heap_ptr(new_native(
                u"toString",
                [](Interpreter& in, Value t,
                   std::vector<Value>&) -> Value {
                    Value symbol = t;
                    if (!symbol.is_heap_ptr() ||
                        symbol.as_heap_ptr()->kind != HeapObject::kJSSymbol)
                        symbol = wrapped_primitive(t);
                    return in.str(
                        in.symbol_descriptive_string(symbol));
                })),
            false);
    }

    // ---- wire constructor.prototype <-> prototype.constructor for core builtins ----
    {
        auto wire = [&](const char16_t* name, JSObject* proto) {
            Value* c = global_->find(name);
            if (c && c->is_heap_ptr() && c->as_heap_ptr()->kind == HeapObject::kJSFunction) {
                auto* ctor = static_cast<JSFunction*>(c->as_heap_ptr());
                ctor->set(u"prototype", Value::make_heap_ptr(proto), false);
                proto->set(u"constructor", Value::make_heap_ptr(ctor), false);
            }
        };
        wire(u"Object", object_proto_);
        wire(u"Array", array_proto_);
        wire(u"Function", function_proto_);
        wire(u"Number", number_proto_);
        wire(u"BigInt", bigint_proto_);
        wire(u"Boolean", boolean_proto_);
        wire(u"Promise", promise_proto_);
        wire(u"Map", map_proto_);
        wire(u"Set", set_proto_);
        wire(u"Symbol", symbol_proto_);
        // String ctor is a plain function (def): attach prototype + back-link manually.
        if (Value* c = global_->find(u"String"); c && c->is_heap_ptr() && c->as_heap_ptr()->kind == HeapObject::kJSFunction) {
            auto* ctor = static_cast<JSFunction*>(c->as_heap_ptr());
            ctor->set(u"prototype", Value::make_heap_ptr(string_proto_), false);
            string_proto_->set(u"constructor", Value::make_heap_ptr(ctor), false);
        }
    }

    // ---- Proxy ----
    {
        JSFunction* Proxy = new_native(u"Proxy", [this](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value target = arg(a, 0), handler = arg(a, 1);
            auto is_obj = [](Value v) { return v.is_heap_ptr() && v.as_heap_ptr()->kind != HeapObject::kJSString; };
            if (!is_obj(target) || !is_obj(handler))
                in.throw_error(u"TypeError", u"Cannot create proxy with a non-object as target or handler");
            auto* px = heap_.alloc<JSProxy>();
            px->target = target; px->handler = handler;
            return Value::make_heap_ptr(px);
        }, 2);
        global_->define(u"Proxy", Value::make_heap_ptr(Proxy));
    }

    // ---- Reflect ----
    {
        JSObject* R = new_object();
        auto rm = [&](const char* name, NativeFn fn, uint32_t arity) {
            R->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)));
        };
        rm("get", [](Interpreter& in, Value, std::vector<Value>& a) {
            return in.get_prop_public(arg(a, 0), in.to_property_key(arg(a, 1))); }, 2);
        rm("set", [](Interpreter& in, Value, std::vector<Value>& a) {
            in.set_prop_public(arg(a, 0), in.to_property_key(arg(a, 1)), arg(a, 2)); return Value::make_bool(true); }, 3);
        rm("has", [](Interpreter& in, Value, std::vector<Value>& a) {
            Value o = arg(a, 0); std::u16string k = in.to_property_key(arg(a, 1));
            if (o.is_heap_ptr() && (o.as_heap_ptr()->kind == HeapObject::kJSObject || o.as_heap_ptr()->kind == HeapObject::kJSArray))
                return Value::make_bool(static_cast<JSObject*>(o.as_heap_ptr())->has(k));
            return Value::make_bool(false); }, 2);
        rm("deleteProperty", [](Interpreter& in, Value, std::vector<Value>& a) {
            Value o = arg(a, 0);
            if (o.is_heap_ptr() && (o.as_heap_ptr()->kind == HeapObject::kJSObject || o.as_heap_ptr()->kind == HeapObject::kJSArray))
                static_cast<JSObject*>(o.as_heap_ptr())->delete_prop(in.to_property_key(arg(a, 1)));
            return Value::make_bool(true); }, 2);
        rm("ownKeys", [](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* out = in.new_array(); Value o = arg(a, 0);
            while (o.is_heap_ptr() &&
                   o.as_heap_ptr()->kind ==
                       HeapObject::kJSProxy) {
                auto* proxy =
                    static_cast<JSProxy*>(o.as_heap_ptr());
                Value trap = in.get_prop_public(
                    proxy->handler, u"ownKeys");
                if (in.is_callable(trap)) {
                    std::vector<Value> arguments{proxy->target};
                    return in.call(
                        trap, proxy->handler, arguments);
                }
                o = proxy->target;
            }
            if (o.is_heap_ptr() &&
                o.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* array =
                    static_cast<JSArray*>(o.as_heap_ptr());
                for (size_t index = 0;
                     index < array->elements.size(); ++index)
                    if (array->has_index(index))
                        out->append(
                            in.str(std::to_string(index)));
                out->append(in.str("length"));
                for (auto& property : array->props)
                    out->append(in.str(property.key));
            } else if (o.is_heap_ptr() &&
                       (o.as_heap_ptr()->kind ==
                            HeapObject::kJSObject ||
                        o.as_heap_ptr()->kind ==
                            HeapObject::kJSFunction)) {
                for (auto& property :
                     static_cast<JSObject*>(
                         o.as_heap_ptr())->props)
                    out->append(in.str(property.key));
            }
            return Value::make_heap_ptr(out); }, 1);
        rm("getPrototypeOf", [](Interpreter&, Value, std::vector<Value>& a) -> Value {
            Value o = arg(a, 0);
            if (JSObject* oo = as_proto_object(o)) {
                JSObject* p = oo->proto;
                return p ? Value::make_heap_ptr(p) : Value::make_null();
            }
            return Value::make_null(); }, 1);
        rm("setPrototypeOf", [](Interpreter&, Value, std::vector<Value>& a) -> Value {
            Value o = arg(a, 0), proto = arg(a, 1);
            if (JSObject* oo = as_proto_object(o))
                oo->proto = as_proto_object(proto);
            return Value::make_bool(true); }, 2);
        rm("apply", [](Interpreter& in, Value, std::vector<Value>& a) {
            std::vector<Value> args = in.to_values(arg(a, 2));
            return in.call(arg(a, 0), arg(a, 1), args); }, 3);
        rm("construct", [](Interpreter& in, Value, std::vector<Value>& a) {
            std::vector<Value> args = in.to_values(arg(a, 1));
            return in.construct(arg(a, 0), args); }, 2);
        // defineProperty / getOwnPropertyDescriptor delegate to Object's versions.
        if (Value* objc = global_->find(u"Object"); objc && objc->is_heap_ptr() && objc->as_heap_ptr()->kind == HeapObject::kJSFunction) {
            auto* O = static_cast<JSFunction*>(objc->as_heap_ptr());
            R->set(u"defineProperty", O->get(u"defineProperty"));
            R->set(u"getOwnPropertyDescriptor", O->get(u"getOwnPropertyDescriptor"));
        }
        global_->define(u"Reflect", Value::make_heap_ptr(R));
    }

    install_array_proto();
    install_string_proto();
    install_promise_proto();
    install_typed_arrays();
    install_intl();
}

void Interpreter::install_promise_proto() {
    auto m = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
        promise_proto_->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
    };
    m("then", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        if (!in.is_promise(t)) return Value::make_undefined();
        return in.promise_then(static_cast<JSPromise*>(t.as_heap_ptr()), arg(a, 0), arg(a, 1));
    }, 2);
    m("catch", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        if (!in.is_promise(t)) return Value::make_undefined();
        return in.promise_then(static_cast<JSPromise*>(t.as_heap_ptr()), Value::make_undefined(), arg(a, 0));
    }, 1);
    m("finally", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        if (!in.is_promise(t)) return Value::make_undefined();
        Value on_finally = arg(a, 0);
        JSFunction* onF = in.new_native(u"", nullptr);
        onF->set(u"%onFinally%", on_finally, false);
        onF->native = [onF](Interpreter& in2, Value, std::vector<Value>& ra) {
            Value callback = onF->get(u"%onFinally%");
            if (in2.is_callable(callback)) {
                std::vector<Value> e;
                in2.call(callback, Value::make_undefined(), e);
            }
            return ra.empty() ? Value::make_undefined() : ra[0];
        };
        JSFunction* onR = in.new_native(u"", nullptr);
        onR->set(u"%onFinally%", on_finally, false);
        onR->native = [onR](Interpreter& in2, Value, std::vector<Value>& ra) -> Value {
            Value callback = onR->get(u"%onFinally%");
            if (in2.is_callable(callback)) {
                std::vector<Value> e;
                in2.call(callback, Value::make_undefined(), e);
            }
            throw ThrowSignal{ra.empty() ? Value::make_undefined() : ra[0]};
        };
        return in.promise_then(static_cast<JSPromise*>(t.as_heap_ptr()), Value::make_heap_ptr(onF), Value::make_heap_ptr(onR));
    }, 1);
}

// ---- Array.prototype ----
void Interpreter::install_array_proto() {
    auto m = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
        array_proto_->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
    };
    m("push", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        if (t.is_null() || t.is_undefined())
            in.throw_error(u"TypeError", u"Array.prototype.push called on null or undefined");

        constexpr double kMaxSafeInteger = 9007199254740991.0;
        auto result_length = [](double length) {
            if (length <= static_cast<double>(std::numeric_limits<int32_t>::max()))
                return Value::make_int32(static_cast<int32_t>(length));
            return Value::make_double(length);
        };

        if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSArray) {
            auto* array = static_cast<JSArray*>(t.as_heap_ptr());
            if (!array->length_writable)
                in.throw_error(u"TypeError", u"Cannot assign to read only property 'length'");
            if (a.size() > 0xffffffffULL - array->elements.size())
                in.throw_error(u"RangeError", u"Invalid array length");
            for (Value value : a) array->append(value);
            return result_length(static_cast<double>(array->elements.size()));
        }

        double length = in.to_number(in.get_prop_public(t, u"length"));
        if (std::isnan(length) || length <= 0) length = 0;
        else if (!std::isfinite(length) || length > kMaxSafeInteger) length = kMaxSafeInteger;
        else length = std::floor(length);
        if (static_cast<double>(a.size()) > kMaxSafeInteger - length)
            in.throw_error(u"TypeError", u"Array.prototype.push exceeded the maximum safe integer");

        for (size_t i = 0; i < a.size(); ++i) {
            uint64_t index = static_cast<uint64_t>(length) + i;
            in.set_prop_public(t, u16(std::to_string(index)), a[i]);
        }
        length += static_cast<double>(a.size());
        in.set_prop_public(t, u"length", result_length(length));
        return result_length(length);
    }, 1);
    m("pop", [](Interpreter& in, Value t, std::vector<Value>&) {
        JSArray* arr = as_array(in, t); if (arr->elements.empty()) return Value::make_undefined();
        size_t index = arr->elements.size() - 1;
        Value v = arr->has_index(index) ? arr->elements[index] : Value::make_undefined();
        arr->resize_length(index, false);
        return v; });
    m("shift", [](Interpreter& in, Value t, std::vector<Value>&) {
        JSArray* arr = as_array(in, t); if (arr->elements.empty()) return Value::make_undefined();
        Value v = arr->has_index(0) ? arr->elements.front() : Value::make_undefined();
        arr->erase_range(0, 1);
        return v; });
    m("unshift", [](Interpreter& in, Value t, std::vector<Value>& a) {
        JSArray* arr = as_array(in, t); arr->insert_dense(0, a);
        return Value::make_int32(static_cast<int32_t>(arr->elements.size())); }, 1);
    m("indexOf", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value needle = arg(a, 0);
        for (size_t i = 0; i < el.size(); ++i)
            if (element_present(t, i) && in.strict_equals(el[i], needle))
                return Value::make_int32(static_cast<int32_t>(i));
        return Value::make_int32(-1); }, 1);
    m("includes", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value needle = arg(a, 0);
        for (auto& v : el) if (in.strict_equals(v, needle)) return Value::make_bool(true);
        return Value::make_bool(false); }, 1);
    m("join", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t);
        std::u16string sep = (a.empty() || a[0].is_undefined()) ? u"," : in.to_string(a[0]);
        std::u16string out;
        for (size_t i = 0; i < el.size(); ++i) { if (i) out += sep; Value e = el[i]; if (!e.is_null() && !e.is_undefined()) out += in.to_string(e); }
        return in.str(narrow(out)); }, 1);
    array_proto_->set(u"toString", array_proto_->get(u"join"), false);
    m("slice", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); int n = static_cast<int>(el.size());
        int start = a.size() > 0 ? in.to_int32(a[0]) : 0;
        int end = (a.size() > 1 && !a[1].is_undefined()) ? in.to_int32(a[1]) : n;
        if (start < 0) start += n;
        if (end < 0) end += n;
        start = std::clamp(start, 0, n);
        end = std::clamp(end, 0, n);
        JSArray* out = in.new_array();
        for (int i = start; i < end; ++i)
            out->append(el[i], element_present(t, static_cast<size_t>(i)));
        return Value::make_heap_ptr(out); }, 2);
    m("concat", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); JSArray* out = in.new_array();
        for (size_t i = 0; i < el.size(); ++i) out->append(el[i], element_present(t, i));
        for (auto& v : a) {
            if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* o = static_cast<JSArray*>(v.as_heap_ptr());
                for (size_t i = 0; i < o->elements.size(); ++i)
                    out->append(o->elements[i], o->has_index(i));
            } else {
                out->append(v);
            }
        }
        return Value::make_heap_ptr(out); }, 1);
    m("reverse", [](Interpreter& in, Value t, std::vector<Value>&) {
        JSArray* arr = as_array(in, t); arr->reverse_elements(); return t; });
    m("forEach", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            in.call(cb, arg(a, 1), ca);
        }
        return Value::make_undefined(); }, 1);
    m("map", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0); JSArray* out = in.new_array();
        Root keep(in, Value::make_heap_ptr(out));  // out unrooted across the callback's possible GC
        out->resize_length(el.size(), false);
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            out->set_index(i, in.call(cb, arg(a, 1), ca));
        }
        return Value::make_heap_ptr(out); }, 1);
    m("filter", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0); JSArray* out = in.new_array();
        Root keep(in, Value::make_heap_ptr(out));  // out unrooted across the callback's possible GC
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            if (in.to_bool(in.call(cb, arg(a, 1), ca))) out->append(el[i]);
        }
        return Value::make_heap_ptr(out); }, 1);
    m("find", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = 0; i < el.size(); ++i) { std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t}; if (in.to_bool(in.call(cb, arg(a, 1), ca))) return el[i]; }
        return Value::make_undefined(); }, 1);
    m("findIndex", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = 0; i < el.size(); ++i) { std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t}; if (in.to_bool(in.call(cb, arg(a, 1), ca))) return Value::make_int32(static_cast<int32_t>(i)); }
        return Value::make_int32(-1); }, 1);
    m("flat", [](Interpreter& in, Value t, std::vector<Value>&) {
        auto el = elements_of(in, t); JSArray* out = in.new_array();
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            Value e = el[i];
            if (e.is_heap_ptr() && e.as_heap_ptr()->kind == HeapObject::kJSArray)
                for (size_t j = 0; j < static_cast<JSArray*>(e.as_heap_ptr())->elements.size(); ++j) {
                    auto* inner = static_cast<JSArray*>(e.as_heap_ptr());
                    if (inner->has_index(j)) out->append(inner->elements[j]);
                }
            else out->append(e);
        }
        return Value::make_heap_ptr(out); });
    m("flatMap", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0); JSArray* out = in.new_array();
        Root keep(in, Value::make_heap_ptr(out));  // out unrooted across the callback's possible GC
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            Value r = in.call(cb, arg(a, 1), ca);
            if (r.is_heap_ptr() && r.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* inner = static_cast<JSArray*>(r.as_heap_ptr());
                for (size_t j = 0; j < inner->elements.size(); ++j)
                    if (inner->has_index(j)) out->append(inner->elements[j]);
            } else out->append(r);
        }
        return Value::make_heap_ptr(out); }, 1);
    m("fill", [](Interpreter& in, Value t, std::vector<Value>& a) {
        JSArray* arr = as_array(in, t); Value v = arg(a, 0);
        for (size_t i = 0; i < arr->elements.size(); ++i) arr->set_index(i, v);
        return t; }, 1);
    m("lastIndexOf", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value v = arg(a, 0);
        for (size_t i = el.size(); i-- > 0; )
            if (element_present(t, i) && in.strict_equals(el[i], v))
                return Value::make_int32(static_cast<int32_t>(i));
        return Value::make_int32(-1); }, 1);
    m("some", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            if (in.to_bool(in.call(cb, arg(a, 1), ca))) return Value::make_bool(true);
        }
        return Value::make_bool(false); }, 1);
    m("every", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = 0; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            if (!in.to_bool(in.call(cb, arg(a, 1), ca))) return Value::make_bool(false);
        }
        return Value::make_bool(true); }, 1);
    m("reduce", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        size_t i = 0; Value acc;
        if (a.size() > 1) {
            acc = a[1];
        } else {
            while (i < el.size() && !element_present(t, i)) ++i;
            if (i == el.size())
                in.throw_error(u"TypeError", u"Reduce of empty array with no initial value");
            acc = el[i++];
        }
        for (; i < el.size(); ++i) {
            if (!element_present(t, i)) continue;
            std::vector<Value> ca{acc, el[i], Value::make_int32(static_cast<int32_t>(i)), t};
            acc = in.call(cb, Value::make_undefined(), ca);
        }
        return acc; }, 2);
    m("reduceRight", [](Interpreter& in, Value t, std::vector<Value>& a) {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        Value acc;
        long i = static_cast<long>(el.size()) - 1;
        if (a.size() > 1) {
            acc = a[1];
        } else {
            while (i >= 0 && !element_present(t, static_cast<size_t>(i))) --i;
            if (i < 0)
                in.throw_error(u"TypeError", u"Reduce of empty array with no initial value");
            acc = el[static_cast<size_t>(i--)];
        }
        for (; i >= 0; --i) {
            if (!element_present(t, static_cast<size_t>(i))) continue;
            std::vector<Value> ca{acc, el[static_cast<size_t>(i)],
                                  Value::make_int32(static_cast<int32_t>(i)), t};
            acc = in.call(cb, Value::make_undefined(), ca);
        }
        return acc; }, 2);
    m("at", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        auto el = elements_of(in, t); long n = static_cast<long>(el.size());
        long i = static_cast<long>(in.to_number(arg(a, 0))); if (i < 0) i += n;
        return (i >= 0 && i < n) ? el[static_cast<size_t>(i)] : Value::make_undefined(); }, 1);
    m("splice", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSArray* arr = as_array(in, t); int n = static_cast<int>(arr->elements.size());
        int start = a.size() > 0 ? in.to_int32(a[0]) : 0;
        if (start < 0) start = std::max(n + start, 0);
        start = std::min(start, n);
        int delcount = a.size() > 1 ? std::clamp(in.to_int32(a[1]), 0, n - start) : (a.empty() ? 0 : n - start);
        JSArray* removed = in.new_array();
        for (int i = 0; i < delcount; ++i) {
            size_t index = static_cast<size_t>(start + i);
            removed->append(arr->elements[index], arr->has_index(index));
        }
        std::vector<Value> ins(a.begin() + std::min<size_t>(2, a.size()), a.end());
        arr->erase_range(static_cast<size_t>(start), static_cast<size_t>(delcount));
        arr->insert_dense(static_cast<size_t>(start), ins);
        return Value::make_heap_ptr(removed); }, 2);
    m("copyWithin", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSArray* arr = as_array(in, t); int n = static_cast<int>(arr->elements.size());
        auto rel = [&](int v) { if (v < 0) v += n; return std::clamp(v, 0, n); };
        int target = a.size() > 0 ? rel(in.to_int32(a[0])) : 0;
        int from   = a.size() > 1 ? rel(in.to_int32(a[1])) : 0;
        int end    = (a.size() > 2 && !a[2].is_undefined()) ? rel(in.to_int32(a[2])) : n;
        std::vector<Value> seg(arr->elements.begin() + from, arr->elements.begin() + std::max(from, end));
        std::vector<uint8_t> present;
        present.reserve(seg.size());
        for (int i = from; i < std::max(from, end); ++i)
            present.push_back(arr->has_index(static_cast<size_t>(i)) ? 1 : 0);
        for (size_t i = 0; i < seg.size() && target + static_cast<int>(i) < n; ++i) {
            size_t destination = static_cast<size_t>(target) + i;
            if (present[i]) arr->set_index(destination, seg[i]);
            else arr->delete_index(destination);
        }
        return t; }, 2);
    m("findLast", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = el.size(); i-- > 0; ) { std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t}; if (in.to_bool(in.call(cb, arg(a, 1), ca))) return el[i]; }
        return Value::make_undefined(); }, 1);
    m("findLastIndex", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        auto el = elements_of(in, t); Value cb = arg(a, 0);
        for (size_t i = el.size(); i-- > 0; ) { std::vector<Value> ca{el[i], Value::make_int32(static_cast<int32_t>(i)), t}; if (in.to_bool(in.call(cb, arg(a, 1), ca))) return Value::make_int32(static_cast<int32_t>(i)); }
        return Value::make_int32(-1); }, 1);
    m("sort", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSArray* arr = as_array(in, t); Value cmp = arg(a, 0);
        bool has_cmp = cmp.is_heap_ptr() && cmp.as_heap_ptr()->kind == HeapObject::kJSFunction;
        size_t original_length = arr->elements.size();
        std::vector<Value> present;
        present.reserve(original_length);
        for (size_t i = 0; i < original_length; ++i)
            if (arr->has_index(i)) present.push_back(arr->elements[i]);
        std::stable_sort(present.begin(), present.end(), [&](Value x, Value y) {
            if (x.is_undefined()) return false;
            if (y.is_undefined()) return true;
            if (has_cmp) { std::vector<Value> ca{x, y}; return in.to_number(in.call(cmp, Value::make_undefined(), ca)) < 0; }
            return in.to_string(x) < in.to_string(y);
        });
        arr->resize_length(0, false);
        for (Value value : present) arr->append(value);
        while (arr->elements.size() < original_length)
            arr->append(Value::make_undefined(), false);
        return t; }, 1);
    auto make_iterator =
        [](Interpreter& in, Value target, int kind) -> Value {
        if (target.is_null() || target.is_undefined())
            in.throw_error(
                u"TypeError",
                u"Array iterator called on null or undefined");
        JSObject* iterator = in.new_object();
        iterator->set(u"%iterated%", target, false);
        iterator->set(u"%nextIndex%", Value::make_int32(0), false);
        iterator->set(u"%iterationKind%", Value::make_int32(kind), false);
        iterator->set(
            u"next",
            Value::make_heap_ptr(in.new_native(
                u"next",
                [](Interpreter& i2, Value receiver,
                   std::vector<Value>&) -> Value {
                    if (!receiver.is_heap_ptr() ||
                        receiver.as_heap_ptr()->kind !=
                            HeapObject::kJSObject)
                        i2.throw_error(
                            u"TypeError",
                            u"Array Iterator.prototype.next called on "
                            u"incompatible receiver");
                    auto* iterator_object =
                        static_cast<JSObject*>(receiver.as_heap_ptr());
                    Value target_value =
                        iterator_object->get(u"%iterated%");
                    JSObject* result = i2.new_object();
                    if (target_value.is_undefined()) {
                        result->set(
                            u"value", Value::make_undefined());
                        result->set(u"done", Value::make_bool(true));
                        return Value::make_heap_ptr(result);
                    }

                    double raw_length = i2.to_number(
                        i2.get_prop_public(target_value, u"length"));
                    size_t length =
                        !std::isfinite(raw_length) || raw_length <= 0
                            ? 0
                            : static_cast<size_t>(
                                  std::floor(raw_length));
                    size_t index = static_cast<size_t>(std::max(
                        0, i2.to_int32(iterator_object->get(
                               u"%nextIndex%"))));
                    if (index >= length) {
                        iterator_object->set(
                            u"%iterated%", Value::make_undefined(),
                            false);
                        result->set(
                            u"value", Value::make_undefined());
                        result->set(u"done", Value::make_bool(true));
                        return Value::make_heap_ptr(result);
                    }

                    iterator_object->set(
                        u"%nextIndex%",
                        index + 1 <=
                                static_cast<size_t>(
                                    std::numeric_limits<int32_t>::max())
                            ? Value::make_int32(
                                  static_cast<int32_t>(index + 1))
                            : Value::make_double(
                                  static_cast<double>(index + 1)),
                        false);
                    const int kind = i2.to_int32(
                        iterator_object->get(u"%iterationKind%"));
                    Value index_value =
                        index <= static_cast<size_t>(
                                     std::numeric_limits<int32_t>::max())
                            ? Value::make_int32(
                                  static_cast<int32_t>(index))
                            : Value::make_double(
                                  static_cast<double>(index));
                    Value element = i2.get_prop_public(
                        target_value, u16(std::to_string(index)));
                    if (kind == 0) {
                        result->set(u"value", index_value);
                    } else if (kind == 2) {
                        JSArray* pair = i2.new_array();
                        pair->elements = {index_value, element};
                        result->set(
                            u"value", Value::make_heap_ptr(pair));
                    } else {
                        result->set(u"value", element);
                    }
                    result->set(u"done", Value::make_bool(false));
                    return Value::make_heap_ptr(result);
                })));
        iterator->set(
            u"@@iterator",
            Value::make_heap_ptr(in.new_native(
                u"[Symbol.iterator]",
                [](Interpreter&, Value receiver,
                   std::vector<Value>&) { return receiver; })));
        return Value::make_heap_ptr(iterator);
    };
    JSFunction* keys = new_native(
        u"keys",
        [make_iterator](Interpreter& in, Value t,
                        std::vector<Value>&) {
            return make_iterator(in, t, 0);
        });
    JSFunction* values = new_native(
        u"values",
        [make_iterator](Interpreter& in, Value t,
                        std::vector<Value>&) {
            return make_iterator(in, t, 1);
        });
    JSFunction* entries = new_native(
        u"entries",
        [make_iterator](Interpreter& in, Value t,
                        std::vector<Value>&) {
            return make_iterator(in, t, 2);
        });
    array_proto_->set(
        u"keys", Value::make_heap_ptr(keys), false);
    array_proto_->set(
        u"values", Value::make_heap_ptr(values), false);
    array_proto_->set(
        u"entries", Value::make_heap_ptr(entries), false);
    array_proto_->set(
        u"@@iterator", Value::make_heap_ptr(values), false);
}

// ---- String.prototype ----
void Interpreter::install_string_proto() {
    auto m = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
        string_proto_->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
    };
    m("charAt", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); int i = in.to_int32(arg(a, 0));
        if (i < 0 || i >= static_cast<int>(s.size())) return in.str(std::string());
        return in.str(narrow(std::u16string(1, s[i]))); }, 1);
    m("charCodeAt", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); int i = in.to_int32(arg(a, 0));
        if (i < 0 || i >= static_cast<int>(s.size())) return Value::make_double(std::nan(""));
        return Value::make_int32(static_cast<int32_t>(s[i])); }, 1);
    m("@@iterator", [](Interpreter& in, Value t,
                       std::vector<Value>&) -> Value {
        std::u16string string = as_str(in, t);
        JSArray* values = in.new_array();
        for (size_t index = 0; index < string.size();) {
            size_t width = 1;
            char16_t first = string[index];
            if (first >= 0xD800 && first <= 0xDBFF &&
                index + 1 < string.size()) {
                char16_t second = string[index + 1];
                if (second >= 0xDC00 && second <= 0xDFFF)
                    width = 2;
            }
            values->append(in.str(
                narrow(string.substr(index, width))));
            index += width;
        }
        return array_iterator(in, values);
    });
    m("indexOf", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t), n = in.to_string(arg(a, 0));
        double raw = a.size() > 1 ? in.to_number(a[1]) : 0.0;
        if (std::isnan(raw)) raw = 0.0;
        const size_t start = raw <= 0
            ? 0
            : raw >= static_cast<double>(s.size())
                ? s.size()
                : static_cast<size_t>(std::trunc(raw));
        auto p = s.find(n, start);
        return Value::make_int32(
            p == std::u16string::npos ? -1 : static_cast<int32_t>(p));
    }, 1);
    m("lastIndexOf", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t), n = in.to_string(arg(a, 0));
        double raw = a.size() > 1 && !a[1].is_undefined()
            ? in.to_number(a[1])
            : std::numeric_limits<double>::infinity();
        if (std::isnan(raw)) raw = std::numeric_limits<double>::infinity();
        const size_t start = raw <= 0
            ? 0
            : raw >= static_cast<double>(s.size())
                ? s.size()
                : static_cast<size_t>(std::trunc(raw));
        auto p = s.rfind(n, start);
        return Value::make_int32(
            p == std::u16string::npos ? -1 : static_cast<int32_t>(p));
    }, 1);
    m("includes", [](Interpreter& in, Value t, std::vector<Value>& a) {
        return Value::make_bool(as_str(in, t).find(in.to_string(arg(a, 0))) != std::u16string::npos); }, 1);
    m("startsWith", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t), n = in.to_string(arg(a, 0));
        return Value::make_bool(s.rfind(n, 0) == 0); }, 1);
    m("endsWith", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t), n = in.to_string(arg(a, 0));
        return Value::make_bool(s.size() >= n.size() && s.compare(s.size() - n.size(), n.size(), n) == 0); }, 1);
    m("slice", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t);
        const size_t start = relative_string_index(
            a.empty() ? 0.0 : to_integer_or_infinity(in, a[0]),
            s.size());
        const size_t end =
            a.size() < 2 || a[1].is_undefined()
                ? s.size()
                : relative_string_index(
                      to_integer_or_infinity(in, a[1]), s.size());
        return in.str(narrow(
            end > start ? s.substr(start, end - start) : std::u16string()));
    }, 2);
    m("substring", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t);
        size_t start = clamped_string_index(
            a.empty() ? 0.0 : to_integer_or_infinity(in, a[0]),
            s.size());
        size_t end =
            a.size() < 2 || a[1].is_undefined()
                ? s.size()
                : clamped_string_index(
                      to_integer_or_infinity(in, a[1]), s.size());
        if (start > end) std::swap(start, end);
        return in.str(narrow(s.substr(start, end - start)));
    }, 2);
    m("substr", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t);
        const size_t start = relative_string_index(
            a.empty() ? 0.0 : to_integer_or_infinity(in, a[0]),
            s.size());
        const size_t remaining = s.size() - start;
        size_t count = remaining;
        if (a.size() > 1 && !a[1].is_undefined()) {
            const double requested = to_integer_or_infinity(in, a[1]);
            if (requested <= 0.0) {
                count = 0;
            } else if (requested < static_cast<double>(remaining)) {
                count = static_cast<size_t>(requested);
            }
        }
        return in.str(narrow(s.substr(start, count)));
    }, 2);
    NativeFn to_upper = [](Interpreter& in, Value t, std::vector<Value>&) {
        std::u16string s = as_str(in, t);
        for (auto& c : s)
            if (c >= u'a' && c <= u'z') c = c - u'a' + u'A';
        return in.str(narrow(s));
    };
    NativeFn to_lower = [](Interpreter& in, Value t, std::vector<Value>&) {
        std::u16string s = as_str(in, t);
        for (auto& c : s)
            if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a';
        return in.str(narrow(s));
    };
    m("toUpperCase", to_upper);
    m("toLocaleUpperCase", to_upper);
    m("toLowerCase", to_lower);
    m("toLocaleLowerCase", to_lower);
    m("localeCompare", [](Interpreter& in, Value t, std::vector<Value>& a) {
        const std::u16string left = as_str(in, t);
        const std::u16string right = in.to_string(arg(a, 0));
        return Value::make_int32(left < right ? -1 : left > right ? 1 : 0);
    }, 1);
    m("trim", [](Interpreter& in, Value t, std::vector<Value>&) {
        std::u16string s = as_str(in, t);
        size_t b = s.find_first_not_of(u" \t\n\r"); size_t e = s.find_last_not_of(u" \t\n\r");
        return in.str(narrow(b == std::u16string::npos ? std::u16string() : s.substr(b, e - b + 1))); });
    m("trimStart", [](Interpreter& in, Value t, std::vector<Value>&) {
        std::u16string s = as_str(in, t); size_t b = s.find_first_not_of(u" \t\n\r");
        return in.str(narrow(b == std::u16string::npos ? std::u16string() : s.substr(b))); });
    m("trimEnd", [](Interpreter& in, Value t, std::vector<Value>&) {
        std::u16string s = as_str(in, t); size_t e = s.find_last_not_of(u" \t\n\r");
        return in.str(narrow(e == std::u16string::npos ? std::u16string() : s.substr(0, e + 1))); });
    m("padStart", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); size_t target = static_cast<size_t>(std::max(0.0, in.to_number(arg(a, 0))));
        std::u16string pad = a.size() > 1 ? in.to_string(a[1]) : u" "; if (pad.empty() || s.size() >= target) return in.str(narrow(s));
        std::u16string fill; while (fill.size() < target - s.size()) fill += pad; fill.resize(target - s.size());
        return in.str(narrow(fill + s)); }, 1);
    m("padEnd", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); size_t target = static_cast<size_t>(std::max(0.0, in.to_number(arg(a, 0))));
        std::u16string pad = a.size() > 1 ? in.to_string(a[1]) : u" "; if (pad.empty() || s.size() >= target) return in.str(narrow(s));
        std::u16string fill; while (fill.size() < target - s.size()) fill += pad; fill.resize(target - s.size());
        return in.str(narrow(s + fill)); }, 1);
    m("at", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        std::u16string s = as_str(in, t); long n = static_cast<long>(s.size());
        long i = static_cast<long>(in.to_number(arg(a, 0))); if (i < 0) i += n;
        return (i >= 0 && i < n) ? in.str(narrow(std::u16string(1, s[static_cast<size_t>(i)]))) : Value::make_undefined(); }, 1);
    m("codePointAt", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        std::u16string s = as_str(in, t); size_t i = static_cast<size_t>(in.to_number(arg(a, 0)));
        return i < s.size() ? Value::make_int32(static_cast<int32_t>(s[i])) : Value::make_undefined(); }, 1);
    m("normalize", [](Interpreter& in, Value t, std::vector<Value>&) { return in.str(narrow(as_str(in, t))); });
    m("concat", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); for (auto& v : a) s += in.to_string(v); return in.str(narrow(s)); });
    m("repeat", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); int count = in.to_int32(arg(a, 0)); if (count < 0) in.throw_error(u"RangeError", u"Invalid count value");
        std::u16string out; for (int i = 0; i < count; ++i) out += s; return in.str(narrow(out)); }, 1);
    m("split", [](Interpreter& in, Value t, std::vector<Value>& a) {
        std::u16string s = as_str(in, t); JSArray* out = in.new_array();
        if (a.empty() || a[0].is_undefined()) { out->elements.push_back(in.str(narrow(s))); return Value::make_heap_ptr(out); }
        std::u16string src; bool g, ic, ml;
        if (get_regex(in, a[0], src, g, ic, ml)) {  // regex separator
            size_t prev = 0, pos = 0;
            while (pos <= s.size()) {
                regex::Match mr = regex::search(src, s, pos, ic, ml);
                if (!mr.matched || mr.index >= s.size()) break;
                if (mr.end == mr.index) { pos = mr.index + 1; continue; }  // skip zero-width
                out->elements.push_back(in.str(narrow(s.substr(prev, mr.index - prev))));
                prev = mr.end; pos = mr.end;
            }
            out->elements.push_back(in.str(narrow(s.substr(prev))));
            return Value::make_heap_ptr(out);
        }
        std::u16string sep = in.to_string(a[0]);
        if (sep.empty()) { for (char16_t c : s) out->elements.push_back(in.str(narrow(std::u16string(1, c)))); return Value::make_heap_ptr(out); }
        size_t pos = 0, prev = 0;
        while ((pos = s.find(sep, prev)) != std::u16string::npos) { out->elements.push_back(in.str(narrow(s.substr(prev, pos - prev)))); prev = pos + sep.size(); }
        out->elements.push_back(in.str(narrow(s.substr(prev))));
        return Value::make_heap_ptr(out); }, 1);
    auto do_replace = [](Interpreter& in, Value t, std::vector<Value>& a, bool force_all) -> Value {
        std::u16string s = as_str(in, t);
        Value pat = arg(a, 0), repl = arg(a, 1);
        bool repl_is_fn = repl.is_heap_ptr() && repl.as_heap_ptr()->kind == HeapObject::kJSFunction;
        std::u16string src; bool g, ic, ml;
        if (get_regex(in, pat, src, g, ic, ml)) {
            bool all = g || force_all;
            std::u16string out; size_t pos = 0;
            while (pos <= s.size()) {
                regex::Match mr = regex::search(src, s, pos, ic, ml);
                if (!mr.matched) break;
                if (mr.index < pos || mr.index > s.size() ||
                    mr.end < mr.index || mr.end > s.size())
                    break;
                out += s.substr(pos, mr.index - pos);
                std::u16string matched = s.substr(mr.index, mr.end - mr.index);
                if (repl_is_fn) {
                    std::vector<Value> ca; ca.push_back(in.str(narrow(matched)));
                    for (size_t gi = 1; gi < mr.groups.size(); ++gi) { auto [lo, hi] = mr.groups[gi]; ca.push_back(lo < 0 ? Value::make_undefined() : in.str(narrow(s.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo))))); }
                    ca.push_back(Value::make_int32(static_cast<int32_t>(mr.index)));
                    ca.push_back(in.str(narrow(s)));
                    out += in.to_string(in.call(repl, Value::make_undefined(), ca));
                } else {
                    out += expand_replacement(in.to_string(repl), s, mr);
                }
                if (mr.end == mr.index) {
                    if (mr.index == s.size()) {
                        pos = s.size();
                        break;
                    }
                    out += s[mr.index];
                    pos = mr.index + 1;
                } else {
                    pos = mr.end;
                }
                if (!all) { out += s.substr(pos); return in.str(narrow(out)); }
            }
            out += s.substr(pos);
            return in.str(narrow(out));
        }
        // string pattern
        std::u16string from = in.to_string(pat);
        std::u16string out; size_t pos = 0;
        auto emit_repl = [&](size_t at) {
            if (repl_is_fn) {
                std::vector<Value> ca{ in.str(narrow(from)), Value::make_int32(static_cast<int32_t>(at)), in.str(narrow(s)) };
                out += in.to_string(in.call(repl, Value::make_undefined(), ca));
            } else out += in.to_string(repl);
        };
        if (from.empty()) {
            emit_repl(0);
            if (!force_all) {
                out += s;
                return in.str(narrow(out));
            }
            for (size_t i = 0; i < s.size(); ++i) {
                out += s[i];
                emit_repl(i + 1);
            }
            return in.str(narrow(out));
        }
        while (true) {
            size_t p = s.find(from, pos);
            if (p == std::u16string::npos) { out += s.substr(pos); break; }
            out += s.substr(pos, p - pos); emit_repl(p); pos = p + from.size();
            if (!force_all) { out += s.substr(pos); break; }
        }
        return in.str(narrow(out));
    };
    m("replace", [do_replace](Interpreter& in, Value t, std::vector<Value>& a) { return do_replace(in, t, a, false); }, 2);
    m("replaceAll", [do_replace](Interpreter& in, Value t, std::vector<Value>& a) { return do_replace(in, t, a, true); }, 2);
    m("match", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        std::u16string s = as_str(in, t), src; bool g = false, ic = false, ml = false;
        if (!get_regex(in, arg(a, 0), src, g, ic, ml)) { src = in.to_string(arg(a, 0)); }
        if (g) {
            JSArray* out = in.new_array(); size_t pos = 0; bool any = false;
            while (pos <= s.size()) {
                regex::Match mr = regex::search(src, s, pos, ic, ml);
                if (!mr.matched) break;
                any = true;
                out->elements.push_back(in.str(narrow(s.substr(mr.index, mr.end - mr.index))));
                pos = mr.end > mr.index ? mr.end : mr.index + 1;
            }
            return any ? Value::make_heap_ptr(out) : Value::make_null();
        }
        regex::Match mr = regex::search(src, s, 0, ic, ml);
        if (!mr.matched) return Value::make_null();
        JSArray* out = in.new_array();
        for (auto [lo, hi] : mr.groups)
            out->elements.push_back(lo < 0 ? Value::make_undefined() : in.str(narrow(s.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo)))));
        out->set(u"index", Value::make_int32(static_cast<int32_t>(mr.index)));
        out->set(u"input", in.str(narrow(s)));
        return Value::make_heap_ptr(out); }, 1);
    m("search", [](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        std::u16string s = as_str(in, t), src; bool g, ic, ml;
        if (!get_regex(in, arg(a, 0), src, g, ic, ml)) { src = in.to_string(arg(a, 0)); ic = ml = false; }
        regex::Match mr = regex::search(src, s, 0, ic, ml);
        return Value::make_int32(mr.matched ? static_cast<int32_t>(mr.index) : -1); }, 1);
    auto string_value = [](Interpreter& in, Value t, std::vector<Value>&) -> Value {
        if (t.is_heap_ptr() && t.as_heap_ptr()->kind == HeapObject::kJSString)
            return t;
        Value primitive = wrapped_primitive(t);
        if (primitive.is_heap_ptr() && primitive.as_heap_ptr()->kind == HeapObject::kJSString)
            return primitive;
        in.throw_error(u"TypeError", u"String method called on incompatible receiver");
    };
    m("valueOf", string_value);
    m("toString", string_value);
}

// ---- JSON.stringify (subset) ----
std::u16string Interpreter::json_stringify(Value v) {
    if (v.is_undefined()) return u"undefined";
    if (v.is_null()) return u"null";
    if (v.is_bool()) return v.as_bool() ? u"true" : u"false";
    if (v.is_int32() || v.is_double()) return number_to_string(to_number(v));
    if (v.is_heap_ptr()) {
        HeapObject* o = v.as_heap_ptr();
        if (o->kind == HeapObject::kJSBigInt)
            throw_error(u"TypeError", u"Do not know how to serialize a BigInt");
        if (o->kind == HeapObject::kJSString) {
            std::u16string out = u"\"";
            for (char16_t c : static_cast<JSString*>(o)->data) {
                if (c == u'"') out += u"\\\""; else if (c == u'\\') out += u"\\\\";
                else if (c == u'\n') out += u"\\n"; else out += c;
            }
            return out + u"\"";
        }
        if (o->kind == HeapObject::kJSArray) {
            auto* a = static_cast<JSArray*>(o);
            std::u16string out = u"[";
            for (size_t i = 0; i < a->elements.size(); ++i) { if (i) out += u","; out += json_stringify(a->elements[i]); }
            return out + u"]";
        }
        if (o->kind == HeapObject::kJSObject) {
            auto* obj = static_cast<JSObject*>(o);
            std::u16string out = u"{"; bool first = true;
            for (auto& p : obj->props) {
                if (!p.enumerable) continue;
                if (!first) out += u",";
                first = false;
                out += u"\"" + p.key + u"\":" + json_stringify(p.value);
            }
            return out + u"}";
        }
    }
    return u"null";
}

} // namespace malibu::js::runtime
