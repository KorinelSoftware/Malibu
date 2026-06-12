// ICU-backed ECMAScript Intl primitives.

#include "malibu/js/runtime/interpreter.h"

#include <unicode/ucol.h>
#include <unicode/udat.h>
#include <unicode/ulistformatter.h>
#include <unicode/unum.h>
#include <unicode/upluralrules.h>
#include <unicode/ureldatefmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace malibu::js::runtime {
namespace {

Value arg(std::vector<Value>& values, size_t index) {
    return index < values.size() ? values[index] : Value::make_undefined();
}

std::string ascii(Interpreter& interpreter, Value value) {
    std::u16string text = interpreter.to_string(value);
    std::string output;
    output.reserve(text.size());
    for (char16_t code_unit : text)
        output.push_back(static_cast<char>(code_unit & 0xff));
    return output;
}

std::string locale_from(Interpreter& interpreter, Value locales) {
    if (locales.is_heap_ptr() &&
        locales.as_heap_ptr()->kind == HeapObject::kJSArray) {
        auto* array = static_cast<JSArray*>(locales.as_heap_ptr());
        if (!array->elements.empty() && array->has_index(0))
            return ascii(interpreter, array->elements[0]);
    }
    if (!locales.is_undefined() && !locales.is_null())
        return ascii(interpreter, locales);
    return "en-US";
}

std::string option_string(Interpreter& interpreter, Value options,
                          std::u16string_view key,
                          std::string fallback = {}) {
    if (!options.is_heap_ptr()) return fallback;
    Value value = interpreter.get_prop_public(
        options, std::u16string(key));
    return value.is_undefined() ? fallback
                                : ascii(interpreter, value);
}

int option_integer(Interpreter& interpreter, Value options,
                   std::u16string_view key, int fallback) {
    if (!options.is_heap_ptr()) return fallback;
    Value value = interpreter.get_prop_public(
        options, std::u16string(key));
    return value.is_undefined() ? fallback
                                : interpreter.to_int32(value);
}

bool option_boolean(Interpreter& interpreter, Value options,
                    std::u16string_view key, bool fallback) {
    if (!options.is_heap_ptr()) return fallback;
    Value value = interpreter.get_prop_public(
        options, std::u16string(key));
    return value.is_undefined() ? fallback
                                : interpreter.to_bool(value);
}

std::u16string format_number(const std::string& locale,
                             const std::string& style,
                             const std::string& currency,
                             const std::string& unit,
                             int minimum_fraction_digits,
                             int maximum_fraction_digits,
                             bool use_grouping, double number) {
    UErrorCode status = U_ZERO_ERROR;
    UNumberFormatStyle icu_style = UNUM_DECIMAL;
    if (style == "currency") icu_style = UNUM_CURRENCY;
    else if (style == "percent") icu_style = UNUM_PERCENT;
    UNumberFormat* formatter =
        unum_open(icu_style, nullptr, 0, locale.c_str(), nullptr,
                  &status);
    if (U_FAILURE(status) || !formatter) return u"";
    unum_setAttribute(
        formatter, UNUM_GROUPING_USED, use_grouping ? 1 : 0);
    unum_setAttribute(
        formatter, UNUM_MIN_FRACTION_DIGITS,
        std::max(0, minimum_fraction_digits));
    unum_setAttribute(
        formatter, UNUM_MAX_FRACTION_DIGITS,
        std::max(minimum_fraction_digits, maximum_fraction_digits));
    if (style == "currency" && !currency.empty()) {
        std::u16string code(currency.begin(), currency.end());
        status = U_ZERO_ERROR;
        unum_setTextAttribute(
            formatter, UNUM_CURRENCY_CODE,
            reinterpret_cast<const UChar*>(code.data()),
            static_cast<int32_t>(code.size()), &status);
    }

    status = U_ZERO_ERROR;
    int32_t length = unum_formatDouble(
        formatter, number, nullptr, 0, nullptr, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        unum_close(formatter);
        return u"";
    }
    status = U_ZERO_ERROR;
    std::u16string output(static_cast<size_t>(length), u'\0');
    unum_formatDouble(
        formatter, number,
        reinterpret_cast<UChar*>(output.data()), length, nullptr,
        &status);
    unum_close(formatter);
    if (U_FAILURE(status)) return u"";
    if (style == "unit" && unit == "degree") output += u"\u00B0";
    return output;
}

Value one_part(Interpreter& interpreter,
               const std::u16string& formatted,
               const char* type = "literal") {
    JSArray* output = interpreter.new_array();
    JSObject* part = interpreter.new_object();
    part->set(u"type", interpreter.str(std::string(type)));
    part->set(u"value", interpreter.str(formatted));
    output->append(Value::make_heap_ptr(part));
    return Value::make_heap_ptr(output);
}

JSFunction* supported_locales_function(Interpreter& interpreter) {
    return interpreter.new_native(
        u"supportedLocalesOf",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            JSArray* output = in.new_array();
            Value locales = arg(values, 0);
            if (locales.is_heap_ptr() &&
                locales.as_heap_ptr()->kind == HeapObject::kJSArray) {
                auto* source =
                    static_cast<JSArray*>(locales.as_heap_ptr());
                for (size_t i = 0; i < source->elements.size(); ++i)
                    if (source->has_index(i))
                        output->append(source->elements[i]);
            } else if (!locales.is_undefined()) {
                output->append(locales);
            }
            return Value::make_heap_ptr(output);
        },
        1);
}

void install_supported_locales(Interpreter& interpreter,
                               JSFunction* constructor) {
    constructor->set(
        u"supportedLocalesOf",
        Value::make_heap_ptr(
            supported_locales_function(interpreter)));
}

}  // namespace

void Interpreter::install_intl() {
    JSObject* intl = new_object();

    JSFunction* number_format = new_native(
        u"NumberFormat",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            const std::string locale =
                locale_from(in, arg(values, 0));
            Value options = arg(values, 1);
            const std::string style =
                option_string(in, options, u"style", "decimal");
            const std::string currency =
                option_string(in, options, u"currency");
            const std::string unit =
                option_string(in, options, u"unit");
            const std::string unit_display =
                option_string(in, options, u"unitDisplay", "short");
            const std::string sign_display =
                option_string(in, options, u"signDisplay", "auto");
            const int minimum_fraction_digits =
                option_integer(
                    in, options, u"minimumFractionDigits", 0);
            const int maximum_fraction_digits =
                option_integer(
                    in, options, u"maximumFractionDigits", 3);
            const bool use_grouping =
                option_boolean(in, options, u"useGrouping", true);

            auto format =
                [locale, style, currency, unit,
                 minimum_fraction_digits, maximum_fraction_digits,
                 use_grouping](Interpreter& in2, Value,
                               std::vector<Value>& args) -> Value {
                    return in2.str(format_number(
                        locale, style, currency, unit,
                        minimum_fraction_digits,
                        maximum_fraction_digits, use_grouping,
                        in2.to_number(arg(args, 0))));
                };

            JSObject* formatter = in.new_object();
            formatter->set(
                u"format",
                Value::make_heap_ptr(
                    in.new_native(u"format", format, 1)));
            formatter->set(
                u"formatToParts",
                Value::make_heap_ptr(in.new_native(
                    u"formatToParts",
                    [locale, style, currency, unit,
                     minimum_fraction_digits,
                     maximum_fraction_digits,
                     use_grouping](Interpreter& in2, Value,
                                   std::vector<Value>& args) -> Value {
                        return one_part(
                            in2,
                            format_number(
                                locale, style, currency, unit,
                                minimum_fraction_digits,
                                maximum_fraction_digits, use_grouping,
                                in2.to_number(arg(args, 0))),
                            "integer");
                    },
                    1)));
            formatter->set(
                u"formatRange",
                Value::make_heap_ptr(in.new_native(
                    u"formatRange",
                    [locale, style, currency, unit,
                     minimum_fraction_digits,
                     maximum_fraction_digits,
                     use_grouping](Interpreter& in2, Value,
                                   std::vector<Value>& args) -> Value {
                        std::u16string first = format_number(
                            locale, style, currency, unit,
                            minimum_fraction_digits,
                            maximum_fraction_digits, use_grouping,
                            in2.to_number(arg(args, 0)));
                        std::u16string second = format_number(
                            locale, style, currency, unit,
                            minimum_fraction_digits,
                            maximum_fraction_digits, use_grouping,
                            in2.to_number(arg(args, 1)));
                        return in2.str(
                            first + u" \u2013 " + second);
                    },
                    2)));
            formatter->set(
                u"resolvedOptions",
                Value::make_heap_ptr(in.new_native(
                    u"resolvedOptions",
                    [locale, style, currency, unit, unit_display,
                     sign_display, minimum_fraction_digits,
                     maximum_fraction_digits, use_grouping](
                        Interpreter& in2, Value,
                        std::vector<Value>&) -> Value {
                        JSObject* result = in2.new_object();
                        result->set(u"locale", in2.str(locale));
                        result->set(
                            u"numberingSystem", in2.str("latn"));
                        result->set(u"style", in2.str(style));
                        if (!currency.empty())
                            result->set(
                                u"currency", in2.str(currency));
                        if (!unit.empty())
                            result->set(u"unit", in2.str(unit));
                        result->set(
                            u"unitDisplay", in2.str(unit_display));
                        result->set(
                            u"signDisplay", in2.str(sign_display));
                        result->set(
                            u"minimumFractionDigits",
                            Value::make_int32(
                                minimum_fraction_digits));
                        result->set(
                            u"maximumFractionDigits",
                            Value::make_int32(
                                maximum_fraction_digits));
                        result->set(
                            u"useGrouping",
                            Value::make_bool(use_grouping));
                        return Value::make_heap_ptr(result);
                    })));
            return Value::make_heap_ptr(formatter);
        });
    install_supported_locales(*this, number_format);
    intl->set(
        u"NumberFormat", Value::make_heap_ptr(number_format));

    JSFunction* date_time_format = new_native(
        u"DateTimeFormat",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            const std::string locale =
                locale_from(in, arg(values, 0));
            auto format =
                [locale](Interpreter& in2, Value,
                         std::vector<Value>& args) -> Value {
                    double millis;
                    if (args.empty() || args[0].is_undefined()) {
                        millis = static_cast<double>(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch())
                                .count());
                    } else {
                        millis = in2.to_number(args[0]);
                    }
                    UErrorCode status = U_ZERO_ERROR;
                    UDateFormat* formatter = udat_open(
                        UDAT_DEFAULT, UDAT_DEFAULT, locale.c_str(),
                        nullptr, 0, nullptr, 0, &status);
                    if (U_FAILURE(status) || !formatter)
                        return in2.str("");
                    status = U_ZERO_ERROR;
                    int32_t length = udat_format(
                        formatter, millis, nullptr, 0, nullptr,
                        &status);
                    status = U_ZERO_ERROR;
                    std::u16string output(
                        static_cast<size_t>(length), u'\0');
                    udat_format(
                        formatter, millis,
                        reinterpret_cast<UChar*>(output.data()),
                        length, nullptr, &status);
                    udat_close(formatter);
                    return in2.str(
                        U_FAILURE(status) ? std::u16string()
                                          : output);
                };
            JSObject* formatter = in.new_object();
            formatter->set(
                u"format",
                Value::make_heap_ptr(
                    in.new_native(u"format", format, 1)));
            formatter->set(
                u"formatToParts",
                Value::make_heap_ptr(in.new_native(
                    u"formatToParts",
                    [format](Interpreter& in2, Value,
                             std::vector<Value>& args) -> Value {
                        Value result = format(
                            in2, Value::make_undefined(), args);
                        return one_part(
                            in2, in2.to_string(result));
                    },
                    1)));
            formatter->set(
                u"resolvedOptions",
                Value::make_heap_ptr(in.new_native(
                    u"resolvedOptions",
                    [locale](Interpreter& in2, Value,
                             std::vector<Value>&) -> Value {
                        JSObject* result = in2.new_object();
                        result->set(u"locale", in2.str(locale));
                        result->set(
                            u"calendar", in2.str("gregory"));
                        result->set(
                            u"numberingSystem", in2.str("latn"));
                        result->set(u"timeZone", in2.str("UTC"));
                        return Value::make_heap_ptr(result);
                    })));
            return Value::make_heap_ptr(formatter);
        });
    install_supported_locales(*this, date_time_format);
    intl->set(
        u"DateTimeFormat",
        Value::make_heap_ptr(date_time_format));

    JSFunction* collator = new_native(
        u"Collator",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            const std::string locale =
                locale_from(in, arg(values, 0));
            JSObject* result = in.new_object();
            result->set(
                u"compare",
                Value::make_heap_ptr(in.new_native(
                    u"compare",
                    [locale](Interpreter& in2, Value,
                             std::vector<Value>& args) -> Value {
                        UErrorCode status = U_ZERO_ERROR;
                        UCollator* collator =
                            ucol_open(locale.c_str(), &status);
                        if (U_FAILURE(status) || !collator)
                            return Value::make_int32(0);
                        std::u16string left =
                            in2.to_string(arg(args, 0));
                        std::u16string right =
                            in2.to_string(arg(args, 1));
                        UCollationResult compared = ucol_strcoll(
                            collator,
                            reinterpret_cast<const UChar*>(
                                left.data()),
                            static_cast<int32_t>(left.size()),
                            reinterpret_cast<const UChar*>(
                                right.data()),
                            static_cast<int32_t>(right.size()));
                        ucol_close(collator);
                        return Value::make_int32(
                            compared < 0 ? -1
                                         : compared > 0 ? 1 : 0);
                    },
                    2)));
            return Value::make_heap_ptr(result);
        });
    install_supported_locales(*this, collator);
    intl->set(u"Collator", Value::make_heap_ptr(collator));

    JSFunction* plural_rules = new_native(
        u"PluralRules",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            const std::string locale =
                locale_from(in, arg(values, 0));
            JSObject* result = in.new_object();
            result->set(
                u"select",
                Value::make_heap_ptr(in.new_native(
                    u"select",
                    [locale](Interpreter& in2, Value,
                             std::vector<Value>& args) -> Value {
                        UErrorCode status = U_ZERO_ERROR;
                        UPluralRules* rules = uplrules_openForType(
                            locale.c_str(), UPLURAL_TYPE_CARDINAL,
                            &status);
                        if (U_FAILURE(status) || !rules)
                            return in2.str("other");
                        UChar keyword[32] = {};
                        status = U_ZERO_ERROR;
                        int32_t length = uplrules_select(
                            rules, in2.to_number(arg(args, 0)),
                            keyword, 32, &status);
                        uplrules_close(rules);
                        if (U_FAILURE(status)) return in2.str("other");
                        return in2.str(std::u16string(
                            reinterpret_cast<char16_t*>(keyword),
                            static_cast<size_t>(length)));
                    },
                    1)));
            return Value::make_heap_ptr(result);
        });
    install_supported_locales(*this, plural_rules);
    intl->set(
        u"PluralRules", Value::make_heap_ptr(plural_rules));

    JSFunction* relative_time_format = new_native(
        u"RelativeTimeFormat",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            const std::string locale =
                locale_from(in, arg(values, 0));
            JSObject* result = in.new_object();
            result->set(
                u"format",
                Value::make_heap_ptr(in.new_native(
                    u"format",
                    [locale](Interpreter& in2, Value,
                             std::vector<Value>& args) -> Value {
                        const std::string unit =
                            ascii(in2, arg(args, 1));
                        URelativeDateTimeUnit icu_unit =
                            UDAT_REL_UNIT_SECOND;
                        if (unit == "minute" || unit == "minutes")
                            icu_unit = UDAT_REL_UNIT_MINUTE;
                        else if (unit == "hour" || unit == "hours")
                            icu_unit = UDAT_REL_UNIT_HOUR;
                        else if (unit == "day" || unit == "days")
                            icu_unit = UDAT_REL_UNIT_DAY;
                        else if (unit == "week" || unit == "weeks")
                            icu_unit = UDAT_REL_UNIT_WEEK;
                        else if (unit == "month" || unit == "months")
                            icu_unit = UDAT_REL_UNIT_MONTH;
                        else if (unit == "year" || unit == "years")
                            icu_unit = UDAT_REL_UNIT_YEAR;
                        UErrorCode status = U_ZERO_ERROR;
                        URelativeDateTimeFormatter* formatter =
                            ureldatefmt_open(
                                locale.c_str(), nullptr,
                                UDAT_STYLE_LONG,
                                UDISPCTX_CAPITALIZATION_NONE,
                                &status);
                        if (U_FAILURE(status) || !formatter)
                            return in2.str("");
                        status = U_ZERO_ERROR;
                        int32_t length =
                            ureldatefmt_formatNumeric(
                                formatter,
                                in2.to_number(arg(args, 0)),
                                icu_unit, nullptr, 0, &status);
                        status = U_ZERO_ERROR;
                        std::u16string output(
                            static_cast<size_t>(length), u'\0');
                        ureldatefmt_formatNumeric(
                            formatter,
                            in2.to_number(arg(args, 0)), icu_unit,
                            reinterpret_cast<UChar*>(output.data()),
                            length, &status);
                        ureldatefmt_close(formatter);
                        return in2.str(
                            U_FAILURE(status)
                                ? std::u16string()
                                : output);
                    },
                    2)));
            return Value::make_heap_ptr(result);
        });
    install_supported_locales(*this, relative_time_format);
    intl->set(
        u"RelativeTimeFormat",
        Value::make_heap_ptr(relative_time_format));

    JSFunction* list_format = new_native(
        u"ListFormat",
        [](Interpreter& in, Value,
           std::vector<Value>& values) -> Value {
            const std::string locale =
                locale_from(in, arg(values, 0));
            JSObject* result = in.new_object();
            result->set(
                u"format",
                Value::make_heap_ptr(in.new_native(
                    u"format",
                    [locale](Interpreter& in2, Value,
                             std::vector<Value>& args) -> Value {
                        std::vector<Value> items =
                            in2.to_values(arg(args, 0));
                        std::vector<std::u16string> strings;
                        std::vector<const UChar*> pointers;
                        std::vector<int32_t> lengths;
                        strings.reserve(items.size());
                        for (Value item : items)
                            strings.push_back(in2.to_string(item));
                        for (const auto& string : strings) {
                            pointers.push_back(
                                reinterpret_cast<const UChar*>(
                                    string.data()));
                            lengths.push_back(
                                static_cast<int32_t>(
                                    string.size()));
                        }
                        UErrorCode status = U_ZERO_ERROR;
                        UListFormatter* formatter =
                            ulistfmt_open(locale.c_str(), &status);
                        if (U_FAILURE(status) || !formatter)
                            return in2.str("");
                        status = U_ZERO_ERROR;
                        int32_t length = ulistfmt_format(
                            formatter, pointers.data(),
                            lengths.data(),
                            static_cast<int32_t>(
                                pointers.size()),
                            nullptr, 0, &status);
                        status = U_ZERO_ERROR;
                        std::u16string output(
                            static_cast<size_t>(length), u'\0');
                        ulistfmt_format(
                            formatter, pointers.data(),
                            lengths.data(),
                            static_cast<int32_t>(
                                pointers.size()),
                            reinterpret_cast<UChar*>(
                                output.data()),
                            length, &status);
                        ulistfmt_close(formatter);
                        return in2.str(
                            U_FAILURE(status)
                                ? std::u16string()
                                : output);
                    },
                    1)));
            return Value::make_heap_ptr(result);
        });
    install_supported_locales(*this, list_format);
    intl->set(
        u"ListFormat", Value::make_heap_ptr(list_format));

    intl->set(
        u"getCanonicalLocales",
        Value::make_heap_ptr(supported_locales_function(*this)));
    global_->define(u"Intl", Value::make_heap_ptr(intl));
}

}  // namespace malibu::js::runtime
