// js/engine.cpp
#include <cstdio>
#include <cstdlib>
#include <functional>
// MalibuJS engine facade.

#include "malibu/js/engine.h"
#include "malibu/js/parser/parser.h"

namespace malibu::js {

namespace {
std::string narrow(const std::u16string& s) {
    std::string r; r.reserve(s.size());
    for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF));
    return r;
}

// Proper UTF-16 -> UTF-8 (handles surrogate pairs), used to hand eval/Function
// source text to the byte-oriented parser without mangling non-ASCII.
std::string to_utf8(const std::u16string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string widen(const std::string& s) {
    std::u16string r; r.reserve(s.size());
    for (unsigned char c : s) r.push_back(static_cast<char16_t>(c));
    return r;
}
}  // namespace

Engine::Engine() : interp_(heap_) {
    interp_.set_event_loop(&loop_);
    loop_.set_microtask_drainer([this] { interp_.run_microtasks(); });

    // eval / Function support: parse + compile + run a source string in the
    // global realm. Parse/compile failures surface as JS SyntaxErrors.
    interp_.set_eval_hook([this](const std::u16string& src) -> runtime::Value {
        std::string source = to_utf8(src);
        parser::Parser parser;
        auto parsed = parser.parse(source, "<eval>");
        if (!parsed.ok()) {
            const auto& e = parsed.errors.front();
            interp_.throw_error(u"SyntaxError", widen(e.message));
        }
        compiler::Compiler compiler;
        auto compiled = compiler.compile(*parsed.program);
        if (!compiled.ok()) {
            interp_.throw_error(u"SyntaxError", widen(compiled.error));
        }
        programs_.push_back(compiled.function);
        return interp_.run_program(compiled.function.get());
    });
}

Engine::EvalResult Engine::evaluate(std::string_view source, std::string_view filename) {
    EvalResult result;

    parser::Parser parser;
    auto parsed = parser.parse(source, filename);
    if (!parsed.ok()) {
        const auto& e = parsed.errors.front();
        result.error = e.file + ":" + std::to_string(e.line) + ":" +
                       std::to_string(e.column) + ": " + e.message;
        return result;
    }

    compiler::Compiler compiler;
    auto compiled = compiler.compile(*parsed.program);
    if (!compiled.ok()) {
        result.error = "compile error: " + compiled.error;
        return result;
    }
    programs_.push_back(compiled.function);
    if (std::getenv("MALIBU_BC")) {
        std::function<void(const compiler::Function*, int)> walk = [&](const compiler::Function* f, int d) {
            if (f->code.size() > 20000 || f->str_consts.size() > 20000 || d == 0)
                std::fprintf(stderr, "[bc] d=%d code=%zu str=%zu num=%zu regs=%u nested=%zu\n",
                             d, f->code.size(), f->str_consts.size(), f->num_consts.size(),
                             f->num_registers, f->functions.size());
            for (auto& nf : f->functions) walk(nf.get(), d + 1);
        };
        walk(compiled.function.get(), 0);
    }

    try {
        result.value = interp_.run_program(compiled.function.get());
        interp_.run_microtasks();  // settle synchronously-resolvable promises / async
        result.ok = true;
    } catch (runtime::ThrowSignal& sig) {
        runtime::Value ex = sig.value;
        std::u16string msg;
        if (ex.is_heap_ptr() && ex.as_heap_ptr()->kind == vm::HeapObject::kJSObject) {
            auto* obj = static_cast<runtime::JSObject*>(ex.as_heap_ptr());
            runtime::Value name = obj->get(u"name");
            runtime::Value m = obj->get(u"message");
            msg = interp_.to_string(name) + u": " + interp_.to_string(m);
        } else {
            msg = u"Uncaught " + interp_.to_string(ex);
        }
        result.error = narrow(msg);
    }
    return result;
}

std::string Engine::eval_to_string(std::string_view source) {
    EvalResult r = evaluate(source);
    if (!r.ok) return r.error;
    return narrow(interp_.to_string(r.value));
}

} // namespace malibu::js
