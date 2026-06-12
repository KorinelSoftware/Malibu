// wasm/wasm.cpp
// MalibuWASM: binary decoder + structured stack-machine interpreter.

#include "malibu/wasm/wasm.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace malibu::wasm {
namespace {

// ---- LEB128 reader over a byte span ----
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool           bad = false;

    bool eof() const { return p >= end; }
    uint8_t u8() { if (p >= end) { bad = true; return 0; } return *p++; }

    uint64_t uleb() {
        uint64_t result = 0; int shift = 0;
        while (true) {
            if (p >= end) { bad = true; return result; }
            uint8_t b = *p++;
            result |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift >= 64) { bad = true; break; }
        }
        return result;
    }
    int64_t sleb() {
        int64_t result = 0; int shift = 0; uint8_t b = 0;
        do {
            if (p >= end) { bad = true; return result; }
            b = *p++;
            result |= static_cast<int64_t>(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) result |= -(static_cast<int64_t>(1) << shift);
        return result;
    }
    float f32() { float v; if (p + 4 > end) { bad = true; return 0; } std::memcpy(&v, p, 4); p += 4; return v; }
    double f64() { double v; if (p + 8 > end) { bad = true; return 0; } std::memcpy(&v, p, 8); p += 8; return v; }
    std::string name() {
        uint32_t n = static_cast<uint32_t>(uleb());
        std::string s;
        for (uint32_t i = 0; i < n && p < end; ++i) s.push_back(static_cast<char>(*p++));
        return s;
    }
};

ValType val_type(uint8_t b) {
    switch (b) {
        case 0x7F: return ValType::I32;
        case 0x7E: return ValType::I64;
        case 0x7D: return ValType::F32;
        case 0x7C: return ValType::F64;
        case 0x70: return ValType::FuncRef;
        case 0x6F: return ValType::ExternRef;
        default:   return ValType::I32;
    }
}

bool read_i32_offset_expr(Reader& r, int32_t& constant,
                          std::optional<uint32_t>& global,
                          std::string& error) {
    uint8_t op = r.u8();
    if (op == 0x41) {
        constant = static_cast<int32_t>(r.sleb());
    } else if (op == 0x23) {
        global = static_cast<uint32_t>(r.uleb());
    } else {
        error = "unsupported offset expression";
        return false;
    }
    if (r.u8() != 0x0B) {
        error = "unterminated offset expression";
        return false;
    }
    return true;
}

}  // namespace

DecodeResult decode(const uint8_t* bytes, size_t len) {
    DecodeResult res;
    if (len < 8 || bytes[0] != 0x00 || bytes[1] != 0x61 || bytes[2] != 0x73 || bytes[3] != 0x6D) {
        res.error = "invalid magic"; return res;
    }
    uint32_t version; std::memcpy(&version, bytes + 4, 4);
    if (version != 1) { res.error = "unsupported version"; return res; }

    auto m = std::make_unique<Module>();
    Reader r{bytes + 8, bytes + len};

    uint32_t num_imported_funcs = 0;
    while (!r.eof()) {
        uint8_t sec_id = r.u8();
        uint32_t sec_len = static_cast<uint32_t>(r.uleb());
        const uint8_t* sec_end = r.p + sec_len;
        if (sec_end > r.end) { res.error = "section overruns module"; return res; }

        switch (sec_id) {
            case 1: {  // type
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    uint8_t form = r.u8();
                    if (form != 0x60) { res.error = "bad functype form"; return res; }
                    FuncType ft;
                    uint32_t np = static_cast<uint32_t>(r.uleb());
                    for (uint32_t j = 0; j < np; ++j) ft.params.push_back(val_type(r.u8()));
                    uint32_t nr = static_cast<uint32_t>(r.uleb());
                    for (uint32_t j = 0; j < nr; ++j) ft.results.push_back(val_type(r.u8()));
                    m->types.push_back(std::move(ft));
                }
                break;
            }
            case 2: {  // import
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    std::string mod = r.name();
                    std::string nm = r.name();
                    uint8_t kind = r.u8();
                    if (kind == 0) {  // func import
                        uint32_t ti = static_cast<uint32_t>(r.uleb());
                        Func f; f.type_index = ti; f.is_import = true;
                        m->funcs.push_back(std::move(f));
                        m->func_imports.push_back({mod, nm});
                        num_imported_funcs++;
                    } else if (kind == 1) {  // table
                        Table table;
                        table.element_type = r.u8();
                        uint32_t flags = static_cast<uint32_t>(r.uleb());
                        table.min_size = static_cast<uint32_t>(r.uleb());
                        table.has_max = (flags & 1U) != 0;
                        if (table.has_max)
                            table.max_size = static_cast<uint32_t>(r.uleb());
                        table.is_import = true;
                        m->tables.push_back(table);
                    } else if (kind == 2) {  // memory
                        uint32_t flags = static_cast<uint32_t>(r.uleb());
                        m->mem_min_pages = static_cast<uint32_t>(r.uleb());
                        if (flags & 1U)
                            m->mem_max_pages = static_cast<uint32_t>(r.uleb());
                        m->has_memory = true;
                    } else if (kind == 3) {  // global
                        r.u8(); r.u8();
                    }
                }
                break;
            }
            case 3: {  // function
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    Func f; f.type_index = static_cast<uint32_t>(r.uleb());
                    m->funcs.push_back(std::move(f));
                }
                break;
            }
            case 4: {  // table
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    Table table;
                    table.element_type = r.u8();
                    uint32_t flags = static_cast<uint32_t>(r.uleb());
                    table.min_size = static_cast<uint32_t>(r.uleb());
                    table.has_max = (flags & 1U) != 0;
                    if (table.has_max)
                        table.max_size = static_cast<uint32_t>(r.uleb());
                    m->tables.push_back(table);
                }
                break;
            }
            case 5: {  // memory
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t flags = static_cast<uint32_t>(r.uleb());
                    m->mem_min_pages = static_cast<uint32_t>(r.uleb());
                    if (flags & 1U)
                        m->mem_max_pages = static_cast<uint32_t>(r.uleb());
                    m->has_memory = true;
                }
                break;
            }
            case 6: {  // global
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    Global g; g.type = val_type(r.u8()); g.mutable_ = r.u8() != 0;
                    uint8_t op = r.u8();
                    if (op == 0x41) g.value = Value::I32(static_cast<int32_t>(r.sleb()));
                    else if (op == 0x42) g.value = Value::I64(r.sleb());
                    else if (op == 0x43) g.value = Value::F32(r.f32());
                    else if (op == 0x44) g.value = Value::F64(r.f64());
                    r.u8();  // end (0x0B)
                    m->globals.push_back(g);
                }
                break;
            }
            case 7: {  // export
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    Export e; e.name = r.name(); e.kind = r.u8();
                    e.index = static_cast<uint32_t>(r.uleb());
                    m->exports.push_back(std::move(e));
                }
                break;
            }
            case 8:  // start
                m->start_func = static_cast<int>(r.uleb());
                break;
            case 9: {  // element
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    ElementSegment segment;
                    uint32_t flags = static_cast<uint32_t>(r.uleb());
                    if (flags > 7) {
                        res.error = "unsupported element segment flags";
                        return res;
                    }

                    bool uses_expressions = (flags & 4U) != 0;
                    switch (flags) {
                        case 0:
                        case 4:
                            break;
                        case 1:
                        case 5:
                            segment.mode = ElementSegment::Mode::Passive;
                            break;
                        case 2:
                        case 6:
                            segment.table_index =
                                static_cast<uint32_t>(r.uleb());
                            break;
                        case 3:
                        case 7:
                            segment.mode =
                                ElementSegment::Mode::Declarative;
                            break;
                    }

                    if (segment.mode == ElementSegment::Mode::Active &&
                        !read_i32_offset_expr(
                            r, segment.constant_offset,
                            segment.offset_global, res.error))
                        return res;

                    if (uses_expressions) {
                        if (flags != 4)
                            segment.element_type = r.u8();
                    } else if (flags != 0) {
                        if (r.u8() != 0x00) {
                            res.error = "invalid element kind";
                            return res;
                        }
                    }

                    uint32_t count = static_cast<uint32_t>(r.uleb());
                    segment.function_indices.reserve(count);
                    for (uint32_t j = 0; j < count; ++j) {
                        if (!uses_expressions) {
                            segment.function_indices.push_back(
                                static_cast<uint32_t>(r.uleb()));
                            continue;
                        }
                        uint8_t op = r.u8();
                        if (op == 0xD2) {
                            segment.function_indices.push_back(
                                static_cast<uint32_t>(r.uleb()));
                        } else if (op == 0xD0) {
                            r.u8();  // reference type
                            segment.function_indices.push_back(std::nullopt);
                        } else {
                            res.error =
                                "unsupported element initializer expression";
                            return res;
                        }
                        if (r.u8() != 0x0B) {
                            res.error =
                                "unterminated element initializer expression";
                            return res;
                        }
                    }
                    m->element_segments.push_back(std::move(segment));
                }
                break;
            }
            case 10: {  // code
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t body_size = static_cast<uint32_t>(r.uleb());
                    const uint8_t* body_start = r.p;
                    const uint8_t* body_end = r.p + body_size;
                    // locals
                    uint32_t local_decls = static_cast<uint32_t>(r.uleb());
                    Func& f = m->funcs[num_imported_funcs + i];
                    for (uint32_t d = 0; d < local_decls; ++d) {
                        uint32_t count = static_cast<uint32_t>(r.uleb());
                        ValType t = val_type(r.u8());
                        for (uint32_t c = 0; c < count; ++c) f.locals.push_back(t);
                    }
                    f.code.assign(r.p, body_end);
                    r.p = body_end;
                    (void)body_start;
                }
                break;
            }
            case 11: {  // data
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    DataSegment segment;
                    uint32_t flags = static_cast<uint32_t>(r.uleb());
                    if (flags == 1) {
                        segment.passive = true;
                    } else if (flags == 0 || flags == 2) {
                        if (flags == 2)
                            segment.memory_index =
                                static_cast<uint32_t>(r.uleb());
                        if (!read_i32_offset_expr(
                                r, segment.constant_offset,
                                segment.offset_global, res.error))
                            return res;
                    } else {
                        res.error = "unsupported data segment flags";
                        return res;
                    }
                    uint64_t byte_count = r.uleb();
                    if (byte_count >
                        static_cast<uint64_t>(sec_end - r.p)) {
                        res.error = "data segment overruns section";
                        return res;
                    }
                    segment.bytes.assign(
                        r.p, r.p + static_cast<size_t>(byte_count));
                    r.p += static_cast<size_t>(byte_count);
                    m->data_segments.push_back(std::move(segment));
                }
                break;
            }
            default:  // custom / unsupported section: skip
                r.p = sec_end;
                break;
        }
        if (r.bad) { res.error = "truncated section"; return res; }
        r.p = sec_end;  // be robust to per-section trailing bytes
    }

    res.module = std::move(m);
    return res;
}

// ---------------------------------------------------------------------------
// Interpreter — recursive, structured. Control transfer (br/return) is carried
// up the recursion via a Signal.
// ---------------------------------------------------------------------------
struct Signal {
    enum Kind { Normal, Branch, Return } kind = Normal;
    uint32_t depth = 0;  // for Branch: number of label levels to skip
};

struct Interp {
    Instance& inst;
    const Module& mod;
    std::string& err;
    bool failed = false;
    std::vector<Value> stack;
    std::vector<uint32_t> call_stack;
    std::vector<std::string> host_history;

    Interp(Instance& i, const Module& m, std::string& e) : inst(i), mod(m), err(e) {}

    void push(Value v) { stack.push_back(v); }
    Value pop() { if (stack.empty()) { fail("stack underflow"); return Value::I32(0); } Value v = stack.back(); stack.pop_back(); return v; }
    int32_t popi() { return pop().i32; }
    void fail(const std::string& m) { if (!failed) { err = m; failed = true; } }

    Memory& mem() { return inst.memory_; }

    // Skip an instruction's immediates without executing (used to find else/end).
    // We instead use a structured executor that consumes a body until its end.

    // Execute the instruction sequence of `code` starting at *pc until the
    // matching `end`/`else` at this nesting level. Returns a Signal.
    Signal run(const std::vector<uint8_t>& code, size_t& pc, std::vector<Value>& locals, int stop_at_else) {
        Reader r{code.data() + pc, code.data() + code.size()};
        Signal sig;
        while (!r.eof() && !failed) {
            uint8_t op = r.u8();
            switch (op) {
                case 0x0B: pc = r.p - code.data(); return sig;             // end
                case 0x05: pc = r.p - code.data(); sig.kind = Signal::Normal; if (stop_at_else) return sig; break;  // else
                case 0x00: {
                    std::string message = "unreachable";
                    if (!call_stack.empty())
                        message += " in function " +
                                   std::to_string(call_stack.back());
                    message += " at offset " +
                               std::to_string(
                                   static_cast<size_t>(r.p - code.data() - 1));
                    if (call_stack.size() > 1) {
                        message += " (call stack ";
                        for (size_t i = 0; i < call_stack.size(); ++i) {
                            if (i) message += " -> ";
                            message += std::to_string(call_stack[i]);
                        }
                        message += ")";
                    }
                    if (!host_history.empty()) {
                        message += " (recent imports: ";
                        for (size_t i = 0; i < host_history.size(); ++i) {
                            if (i) message += "; ";
                            message += host_history[i];
                        }
                        message += ")";
                    }
                    fail(message);
                    return sig;
                }
                case 0x01: break;                                          // nop
                case 0x02: case 0x03: {                                    // block / loop
                    bool is_loop = (op == 0x03);
                    read_blocktype(r);
                    size_t body_pc = r.p - code.data();
                    while (true) {
                        size_t inner = body_pc;
                        Signal s = run(code, inner, locals, 0);
                        if (failed) return s;
                        if (s.kind == Signal::Return) return s;
                        if (s.kind == Signal::Branch) {
                            if (s.depth == 0) {
                                if (is_loop) continue;     // br 0 in loop => repeat
                                // br 0 in block => exit; advance past matching end
                                r.p = code.data() + skip_to_end(code, body_pc);
                                break;
                            } else { s.depth--; pc = r.p - code.data(); return s; }  // unwind outward
                        }
                        // normal completion: advance reader past the block's end
                        r.p = code.data() + inner;
                        break;
                    }
                    break;
                }
                case 0x04: {                                               // if
                    read_blocktype(r);
                    int32_t cond = popi();
                    size_t then_pc = r.p - code.data();
                    if (cond) {
                        size_t inner = then_pc;
                        Signal s = run(code, inner, locals, 1);
                        if (failed || s.kind != Signal::Normal) { if (s.kind == Signal::Branch && s.depth > 0) s.depth--; return s; }
                        r.p = code.data() + skip_to_end(code, then_pc);
                    } else {
                        size_t else_pc = find_else_or_end(code, then_pc);
                        if (else_pc < code.size() && code[else_pc] == 0x05) {
                            size_t inner = else_pc + 1;
                            Signal s = run(code, inner, locals, 0);
                            if (failed || s.kind != Signal::Normal) { if (s.kind == Signal::Branch && s.depth > 0) s.depth--; return s; }
                            r.p = code.data() + inner;
                        } else {
                            r.p = code.data() + skip_to_end(code, then_pc);
                        }
                    }
                    break;
                }
                case 0x0C: { uint32_t d = (uint32_t)r.uleb(); sig.kind = Signal::Branch; sig.depth = d; pc = r.p - code.data(); return sig; }  // br
                case 0x0D: { uint32_t d = (uint32_t)r.uleb(); if (popi()) { sig.kind = Signal::Branch; sig.depth = d; pc = r.p - code.data(); return sig; } break; }  // br_if
                case 0x0E: {                                               // br_table
                    uint32_t count =
                        static_cast<uint32_t>(r.uleb());
                    std::vector<uint32_t> labels;
                    labels.reserve(count);
                    for (uint32_t i = 0; i < count; ++i)
                        labels.push_back(
                            static_cast<uint32_t>(r.uleb()));
                    uint32_t default_label =
                        static_cast<uint32_t>(r.uleb());
                    uint32_t index =
                        static_cast<uint32_t>(popi());
                    sig.kind = Signal::Branch;
                    sig.depth =
                        index < labels.size()
                            ? labels[index]
                            : default_label;
                    pc = r.p - code.data();
                    return sig;
                }
                case 0x0F: sig.kind = Signal::Return; return sig;          // return
                case 0x10: {                                               // call
                    uint32_t fi = (uint32_t)r.uleb();
                    call_into(fi);
                    if (failed) return sig;
                    break;
                }
                case 0x11: {                                               // call_indirect
                    uint32_t type_index = static_cast<uint32_t>(r.uleb());
                    uint32_t table_index = static_cast<uint32_t>(r.uleb());
                    uint32_t element_index =
                        static_cast<uint32_t>(popi());
                    if (type_index >= mod.types.size() ||
                        table_index >= inst.tables_.size() ||
                        element_index >=
                            inst.tables_[table_index].elements.size()) {
                        fail("undefined element");
                        return sig;
                    }
                    Value reference =
                        inst.tables_[table_index].elements[element_index];
                    if (reference.type != ValType::FuncRef ||
                        reference.ref == 0) {
                        fail("uninitialized element");
                        return sig;
                    }
                    uint64_t function_index = reference.ref - 1;
                    if (function_index >= inst.funcs_.size()) {
                        fail("undefined element");
                        return sig;
                    }
                    const FuncType& expected = mod.types[type_index];
                    const Func& target = inst.funcs_[function_index];
                    if (target.type_index >= mod.types.size() ||
                        !same_type(expected,
                                   mod.types[target.type_index])) {
                        fail("indirect call type mismatch");
                        return sig;
                    }
                    call_into(static_cast<uint32_t>(function_index));
                    if (failed) return sig;
                    break;
                }
                case 0x1A: pop(); break;                                    // drop
                case 0x1B: { Value c = pop(); Value b = pop(); Value a = pop(); push(c.i32 ? a : b); break; }  // select
                // locals / globals
                case 0x20: { uint32_t i = (uint32_t)r.uleb(); if (i < locals.size()) push(locals[i]); else fail("bad local"); break; }
                case 0x21: { uint32_t i = (uint32_t)r.uleb(); if (i < locals.size()) locals[i] = pop(); else fail("bad local"); break; }
                case 0x22: { uint32_t i = (uint32_t)r.uleb(); if (i < locals.size()) locals[i] = stack.back(); else fail("bad local"); break; }
                case 0x23: { uint32_t i = (uint32_t)r.uleb(); if (i < inst.globals_.size()) push(inst.globals_[i].value); else fail("bad global"); break; }
                case 0x24: { uint32_t i = (uint32_t)r.uleb(); if (i < inst.globals_.size()) inst.globals_[i].value = pop(); else fail("bad global"); break; }
                case 0x25: {                                               // table.get
                    uint32_t table_index =
                        static_cast<uint32_t>(r.uleb());
                    uint32_t element_index =
                        static_cast<uint32_t>(popi());
                    if (table_index >= inst.tables_.size() ||
                        element_index >=
                            inst.tables_[table_index].elements.size()) {
                        fail("out of bounds table access");
                        return sig;
                    }
                    push(inst.tables_[table_index].elements[element_index]);
                    break;
                }
                case 0x26: {                                               // table.set
                    uint32_t table_index =
                        static_cast<uint32_t>(r.uleb());
                    Value value = pop();
                    uint32_t element_index =
                        static_cast<uint32_t>(popi());
                    if (table_index >= inst.tables_.size() ||
                        element_index >=
                            inst.tables_[table_index].elements.size()) {
                        fail("out of bounds table access");
                        return sig;
                    }
                    inst.tables_[table_index].elements[element_index] =
                        value;
                    break;
                }
                // constants
                case 0x41: push(Value::I32((int32_t)r.sleb())); break;
                case 0x42: push(Value::I64(r.sleb())); break;
                case 0x43: push(Value::F32(r.f32())); break;
                case 0x44: push(Value::F64(r.f64())); break;
                // Reference Types.
                case 0xD0: {
                    ValType type = val_type(r.u8());
                    if (type != ValType::ExternRef &&
                        type != ValType::FuncRef) {
                        fail("invalid reference type");
                        return sig;
                    }
                    push(Value::Ref(type, 0));
                    break;
                }
                case 0xD1: {
                    Value reference = pop();
                    push(Value::I32(reference.ref == 0));
                    break;
                }
                case 0xD2: {
                    uint32_t function_index =
                        static_cast<uint32_t>(r.uleb());
                    if (function_index >= inst.funcs_.size()) {
                        fail("bad function reference");
                        return sig;
                    }
                    push(Value::Ref(
                        ValType::FuncRef,
                        static_cast<uint64_t>(function_index) + 1));
                    break;
                }
                // Memory operands encode alignment first, then a static offset.
                case 0x28: { uint64_t a = memory_address(r); push(Value::I32(load_mem<int32_t>(a))); break; }
                case 0x29: { uint64_t a = memory_address(r); push(Value::I64(load_mem<int64_t>(a))); break; }
                case 0x2A: { uint64_t a = memory_address(r); push(Value::F32(load_mem<float>(a))); break; }
                case 0x2B: { uint64_t a = memory_address(r); push(Value::F64(load_mem<double>(a))); break; }
                case 0x2C: { uint64_t a = memory_address(r); push(Value::I32(load_mem<int8_t>(a))); break; }
                case 0x2D: { uint64_t a = memory_address(r); push(Value::I32(load_mem<uint8_t>(a))); break; }
                case 0x2E: { uint64_t a = memory_address(r); push(Value::I32(load_mem<int16_t>(a))); break; }
                case 0x2F: { uint64_t a = memory_address(r); push(Value::I32(load_mem<uint16_t>(a))); break; }
                case 0x30: { uint64_t a = memory_address(r); push(Value::I64(load_mem<int8_t>(a))); break; }
                case 0x31: { uint64_t a = memory_address(r); push(Value::I64(load_mem<uint8_t>(a))); break; }
                case 0x32: { uint64_t a = memory_address(r); push(Value::I64(load_mem<int16_t>(a))); break; }
                case 0x33: { uint64_t a = memory_address(r); push(Value::I64(load_mem<uint16_t>(a))); break; }
                case 0x34: { uint64_t a = memory_address(r); push(Value::I64(load_mem<int32_t>(a))); break; }
                case 0x35: { uint64_t a = memory_address(r); push(Value::I64(load_mem<uint32_t>(a))); break; }
                case 0x36: { uint32_t off = memory_offset(r); int32_t v = pop().i32; store_mem(effective_address(pop().i32, off), v); break; }
                case 0x37: { uint32_t off = memory_offset(r); int64_t v = pop().i64; store_mem(effective_address(pop().i32, off), v); break; }
                case 0x38: { uint32_t off = memory_offset(r); float v = pop().f32; store_mem(effective_address(pop().i32, off), v); break; }
                case 0x39: { uint32_t off = memory_offset(r); double v = pop().f64; store_mem(effective_address(pop().i32, off), v); break; }
                case 0x3A: { uint32_t off = memory_offset(r); uint8_t v = static_cast<uint8_t>(pop().i32); store_mem(effective_address(pop().i32, off), v); break; }
                case 0x3B: { uint32_t off = memory_offset(r); uint16_t v = static_cast<uint16_t>(pop().i32); store_mem(effective_address(pop().i32, off), v); break; }
                case 0x3C: { uint32_t off = memory_offset(r); uint8_t v = static_cast<uint8_t>(pop().i64); store_mem(effective_address(pop().i32, off), v); break; }
                case 0x3D: { uint32_t off = memory_offset(r); uint16_t v = static_cast<uint16_t>(pop().i64); store_mem(effective_address(pop().i32, off), v); break; }
                case 0x3E: { uint32_t off = memory_offset(r); uint32_t v = static_cast<uint32_t>(pop().i64); store_mem(effective_address(pop().i32, off), v); break; }
                case 0x3F: r.u8(); push(Value::I32((int32_t)(mem().data.size() / Memory::kPageSize))); break;  // memory.size
                case 0x40: {
                    r.u8();
                    int32_t delta = popi();
                    uint64_t old_pages = mem().data.size() / Memory::kPageSize;
                    uint64_t max_pages = mem().max_pages
                        ? mem().max_pages
                        : static_cast<uint64_t>(std::numeric_limits<uint16_t>::max()) + 1;
                    if (delta < 0 ||
                        static_cast<uint64_t>(delta) > max_pages - old_pages) {
                        push(Value::I32(-1));
                        break;
                    }
                    uint64_t new_pages = old_pages + static_cast<uint32_t>(delta);
                    try {
                        mem().data.resize(
                            static_cast<size_t>(new_pages) * Memory::kPageSize,
                            0);
                        push(Value::I32(static_cast<int32_t>(old_pages)));
                    } catch (const std::bad_alloc&) {
                        push(Value::I32(-1));
                    }
                    break;
                }
                case 0xFC: {                                              // Bulk Memory / tables
                    uint32_t subopcode =
                        static_cast<uint32_t>(r.uleb());
                    switch (subopcode) {
                        case 8: {                                         // memory.init
                            uint32_t data_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t memory_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t length =
                                static_cast<uint32_t>(popi());
                            uint32_t source =
                                static_cast<uint32_t>(popi());
                            uint32_t destination =
                                static_cast<uint32_t>(popi());
                            if (memory_index != 0 ||
                                data_index >= mod.data_segments.size()) {
                                fail("bad memory.init segment");
                                return sig;
                            }
                            const DataSegment& segment =
                                mod.data_segments[data_index];
                            size_t source_size =
                                inst.data_dropped_[data_index]
                                    ? 0
                                    : segment.bytes.size();
                            if (source > source_size ||
                                length > source_size - source ||
                                destination > mem().data.size() ||
                                length >
                                    mem().data.size() - destination) {
                                fail("out of bounds memory.init");
                                return sig;
                            }
                            std::copy_n(
                                segment.bytes.begin() + source, length,
                                mem().data.begin() + destination);
                            break;
                        }
                        case 9: {                                         // data.drop
                            uint32_t data_index =
                                static_cast<uint32_t>(r.uleb());
                            if (data_index >=
                                inst.data_dropped_.size()) {
                                fail("bad data.drop segment");
                                return sig;
                            }
                            inst.data_dropped_[data_index] = true;
                            break;
                        }
                        case 10: {                                        // memory.copy
                            uint32_t destination_memory =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t source_memory =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t length =
                                static_cast<uint32_t>(popi());
                            uint32_t source =
                                static_cast<uint32_t>(popi());
                            uint32_t destination =
                                static_cast<uint32_t>(popi());
                            if (destination_memory != 0 ||
                                source_memory != 0 ||
                                source > mem().data.size() ||
                                length > mem().data.size() - source ||
                                destination > mem().data.size() ||
                                length >
                                    mem().data.size() - destination) {
                                fail("out of bounds memory.copy");
                                return sig;
                            }
                            std::memmove(
                                mem().data.data() + destination,
                                mem().data.data() + source, length);
                            break;
                        }
                        case 11: {                                        // memory.fill
                            uint32_t memory_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t length =
                                static_cast<uint32_t>(popi());
                            uint8_t value =
                                static_cast<uint8_t>(popi());
                            uint32_t destination =
                                static_cast<uint32_t>(popi());
                            if (memory_index != 0 ||
                                destination > mem().data.size() ||
                                length >
                                    mem().data.size() - destination) {
                                fail("out of bounds memory.fill");
                                return sig;
                            }
                            std::fill_n(
                                mem().data.begin() + destination, length,
                                value);
                            break;
                        }
                        case 12: {                                        // table.init
                            uint32_t element_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t table_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t length =
                                static_cast<uint32_t>(popi());
                            uint32_t source =
                                static_cast<uint32_t>(popi());
                            uint32_t destination =
                                static_cast<uint32_t>(popi());
                            if (element_index >=
                                    mod.element_segments.size() ||
                                table_index >= inst.tables_.size()) {
                                fail("bad table.init segment");
                                return sig;
                            }
                            const ElementSegment& segment =
                                mod.element_segments[element_index];
                            size_t source_size =
                                inst.element_dropped_[element_index]
                                    ? 0
                                    : segment.function_indices.size();
                            TableStorage& table =
                                inst.tables_[table_index];
                            if (source > source_size ||
                                length > source_size - source ||
                                destination > table.elements.size() ||
                                length >
                                    table.elements.size() - destination) {
                                fail("out of bounds table.init");
                                return sig;
                            }
                            for (uint32_t i = 0; i < length; ++i) {
                                const auto& function =
                                    segment.function_indices[source + i];
                                table.elements[destination + i] =
                                    Value::Ref(
                                        ValType::FuncRef,
                                        function
                                            ? static_cast<uint64_t>(
                                                  *function) +
                                                  1
                                            : 0);
                            }
                            break;
                        }
                        case 13: {                                        // elem.drop
                            uint32_t element_index =
                                static_cast<uint32_t>(r.uleb());
                            if (element_index >=
                                inst.element_dropped_.size()) {
                                fail("bad elem.drop segment");
                                return sig;
                            }
                            inst.element_dropped_[element_index] = true;
                            break;
                        }
                        case 14: {                                        // table.copy
                            uint32_t destination_table =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t source_table =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t length =
                                static_cast<uint32_t>(popi());
                            uint32_t source =
                                static_cast<uint32_t>(popi());
                            uint32_t destination =
                                static_cast<uint32_t>(popi());
                            if (destination_table >=
                                    inst.tables_.size() ||
                                source_table >= inst.tables_.size()) {
                                fail("bad table.copy table");
                                return sig;
                            }
                            auto& destination_elements =
                                inst.tables_[destination_table].elements;
                            auto& source_elements =
                                inst.tables_[source_table].elements;
                            if (source > source_elements.size() ||
                                length >
                                    source_elements.size() - source ||
                                destination >
                                    destination_elements.size() ||
                                length >
                                    destination_elements.size() -
                                        destination) {
                                fail("out of bounds table.copy");
                                return sig;
                            }
                            std::vector<Value> copied(
                                source_elements.begin() + source,
                                source_elements.begin() + source + length);
                            std::copy(
                                copied.begin(), copied.end(),
                                destination_elements.begin() + destination);
                            break;
                        }
                        case 15: {                                        // table.grow
                            uint32_t table_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t delta =
                                static_cast<uint32_t>(popi());
                            Value initial = pop();
                            if (table_index >= inst.tables_.size()) {
                                fail("bad table.grow table");
                                return sig;
                            }
                            TableStorage& table =
                                inst.tables_[table_index];
                            uint64_t old_size = table.elements.size();
                            uint64_t new_size = old_size + delta;
                            if ((table.has_max &&
                                 new_size > table.max_size) ||
                                new_size >
                                    table.elements.max_size()) {
                                push(Value::I32(-1));
                                break;
                            }
                            try {
                                table.elements.resize(
                                    static_cast<size_t>(new_size),
                                    initial);
                                push(Value::I32(
                                    static_cast<int32_t>(old_size)));
                            } catch (const std::bad_alloc&) {
                                push(Value::I32(-1));
                            }
                            break;
                        }
                        case 16: {                                        // table.size
                            uint32_t table_index =
                                static_cast<uint32_t>(r.uleb());
                            if (table_index >= inst.tables_.size()) {
                                fail("bad table.size table");
                                return sig;
                            }
                            push(Value::I32(static_cast<int32_t>(
                                inst.tables_[table_index]
                                    .elements.size())));
                            break;
                        }
                        case 17: {                                        // table.fill
                            uint32_t table_index =
                                static_cast<uint32_t>(r.uleb());
                            uint32_t length =
                                static_cast<uint32_t>(popi());
                            Value value = pop();
                            uint32_t destination =
                                static_cast<uint32_t>(popi());
                            if (table_index >= inst.tables_.size()) {
                                fail("bad table.fill table");
                                return sig;
                            }
                            auto& elements =
                                inst.tables_[table_index].elements;
                            if (destination > elements.size() ||
                                length >
                                    elements.size() - destination) {
                                fail("out of bounds table.fill");
                                return sig;
                            }
                            std::fill_n(
                                elements.begin() + destination, length,
                                value);
                            break;
                        }
                        default:
                            fail("unsupported 0xfc subopcode " +
                                 std::to_string(subopcode));
                            return sig;
                    }
                    break;
                }
                // i32 comparisons
                case 0x45: push(Value::I32(popi() == 0)); break;           // eqz
                case 0x46: { int32_t b = popi(), a = popi(); push(Value::I32(a == b)); break; }
                case 0x47: { int32_t b = popi(), a = popi(); push(Value::I32(a != b)); break; }
                case 0x48: { int32_t b = popi(), a = popi(); push(Value::I32(a < b)); break; }   // lt_s
                case 0x49: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); push(Value::I32(a < b)); break; }  // lt_u
                case 0x4A: { int32_t b = popi(), a = popi(); push(Value::I32(a > b)); break; }   // gt_s
                case 0x4B: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); push(Value::I32(a > b)); break; }  // gt_u
                case 0x4C: { int32_t b = popi(), a = popi(); push(Value::I32(a <= b)); break; }  // le_s
                case 0x4D: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); push(Value::I32(a <= b)); break; }
                case 0x4E: { int32_t b = popi(), a = popi(); push(Value::I32(a >= b)); break; }  // ge_s
                case 0x4F: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); push(Value::I32(a >= b)); break; }
                // i64 comparisons
                case 0x50: push(Value::I32(pop().i64 == 0)); break;
                case 0x51: { int64_t b = pop().i64, a = pop().i64; push(Value::I32(a == b)); break; }
                case 0x52: { int64_t b = pop().i64, a = pop().i64; push(Value::I32(a != b)); break; }
                case 0x53: { int64_t b = pop().i64, a = pop().i64; push(Value::I32(a < b)); break; }
                case 0x54: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I32(a < b)); break; }
                case 0x55: { int64_t b = pop().i64, a = pop().i64; push(Value::I32(a > b)); break; }
                case 0x56: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I32(a > b)); break; }
                case 0x57: { int64_t b = pop().i64, a = pop().i64; push(Value::I32(a <= b)); break; }
                case 0x58: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I32(a <= b)); break; }
                case 0x59: { int64_t b = pop().i64, a = pop().i64; push(Value::I32(a >= b)); break; }
                case 0x5A: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I32(a >= b)); break; }
                // floating-point comparisons
                case 0x5B: { float b = pop().f32, a = pop().f32; push(Value::I32(a == b)); break; }
                case 0x5C: { float b = pop().f32, a = pop().f32; push(Value::I32(a != b)); break; }
                case 0x5D: { float b = pop().f32, a = pop().f32; push(Value::I32(a < b)); break; }
                case 0x5E: { float b = pop().f32, a = pop().f32; push(Value::I32(a > b)); break; }
                case 0x5F: { float b = pop().f32, a = pop().f32; push(Value::I32(a <= b)); break; }
                case 0x60: { float b = pop().f32, a = pop().f32; push(Value::I32(a >= b)); break; }
                case 0x61: { double b = pop().f64, a = pop().f64; push(Value::I32(a == b)); break; }
                case 0x62: { double b = pop().f64, a = pop().f64; push(Value::I32(a != b)); break; }
                case 0x63: { double b = pop().f64, a = pop().f64; push(Value::I32(a < b)); break; }
                case 0x64: { double b = pop().f64, a = pop().f64; push(Value::I32(a > b)); break; }
                case 0x65: { double b = pop().f64, a = pop().f64; push(Value::I32(a <= b)); break; }
                case 0x66: { double b = pop().f64, a = pop().f64; push(Value::I32(a >= b)); break; }
                // i32 arithmetic
                case 0x67: push(Value::I32(std::countl_zero(static_cast<uint32_t>(popi())))); break;
                case 0x68: push(Value::I32(std::countr_zero(static_cast<uint32_t>(popi())))); break;
                case 0x69: push(Value::I32(std::popcount(static_cast<uint32_t>(popi())))); break;
                case 0x6A: { uint32_t b = static_cast<uint32_t>(popi()), a = static_cast<uint32_t>(popi()); push(Value::I32(std::bit_cast<int32_t>(a + b))); break; }
                case 0x6B: { uint32_t b = static_cast<uint32_t>(popi()), a = static_cast<uint32_t>(popi()); push(Value::I32(std::bit_cast<int32_t>(a - b))); break; }
                case 0x6C: { uint32_t b = static_cast<uint32_t>(popi()), a = static_cast<uint32_t>(popi()); push(Value::I32(std::bit_cast<int32_t>(a * b))); break; }
                case 0x6D: { int32_t b = popi(), a = popi(); if (b == 0) { fail("integer divide by zero"); return sig; } if (a == std::numeric_limits<int32_t>::min() && b == -1) { fail("integer overflow"); return sig; } push(Value::I32(a / b)); break; }
                case 0x6E: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); if (b == 0) { fail("div by zero"); return sig; } push(Value::I32((int32_t)(a / b))); break; }  // div_u
                case 0x6F: { int32_t b = popi(), a = popi(); if (b == 0) { fail("rem by zero"); return sig; } push(Value::I32(b == -1 ? 0 : a % b)); break; }  // rem_s
                case 0x70: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); if (b == 0) { fail("rem by zero"); return sig; } push(Value::I32((int32_t)(a % b))); break; }  // rem_u
                case 0x71: { int32_t b = popi(), a = popi(); push(Value::I32(a & b)); break; }
                case 0x72: { int32_t b = popi(), a = popi(); push(Value::I32(a | b)); break; }
                case 0x73: { int32_t b = popi(), a = popi(); push(Value::I32(a ^ b)); break; }
                case 0x74: { uint32_t b = static_cast<uint32_t>(popi()), a = static_cast<uint32_t>(popi()); push(Value::I32(std::bit_cast<int32_t>(a << (b & 31)))); break; }
                case 0x75: { int32_t b = popi(), a = popi(); push(Value::I32(a >> (b & 31))); break; }   // shr_s
                case 0x76: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); push(Value::I32((int32_t)(a >> (b & 31)))); break; }  // shr_u
                case 0x77: { uint32_t b = static_cast<uint32_t>(popi()), a = static_cast<uint32_t>(popi()); push(Value::I32(std::bit_cast<int32_t>(std::rotl(a, static_cast<int>(b & 31))))); break; }
                case 0x78: { uint32_t b = static_cast<uint32_t>(popi()), a = static_cast<uint32_t>(popi()); push(Value::I32(std::bit_cast<int32_t>(std::rotr(a, static_cast<int>(b & 31))))); break; }
                // i64 arithmetic
                case 0x79: push(Value::I64(std::countl_zero(static_cast<uint64_t>(pop().i64)))); break;
                case 0x7A: push(Value::I64(std::countr_zero(static_cast<uint64_t>(pop().i64)))); break;
                case 0x7B: push(Value::I64(std::popcount(static_cast<uint64_t>(pop().i64)))); break;
                case 0x7C: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(a + b))); break; }
                case 0x7D: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(a - b))); break; }
                case 0x7E: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(a * b))); break; }
                case 0x7F: { int64_t b = pop().i64, a = pop().i64; if (b == 0) { fail("integer divide by zero"); return sig; } if (a == std::numeric_limits<int64_t>::min() && b == -1) { fail("integer overflow"); return sig; } push(Value::I64(a / b)); break; }
                case 0x80: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); if (b == 0) { fail("integer divide by zero"); return sig; } push(Value::I64(std::bit_cast<int64_t>(a / b))); break; }
                case 0x81: { int64_t b = pop().i64, a = pop().i64; if (b == 0) { fail("integer divide by zero"); return sig; } push(Value::I64(b == -1 ? 0 : a % b)); break; }
                case 0x82: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); if (b == 0) { fail("integer divide by zero"); return sig; } push(Value::I64(std::bit_cast<int64_t>(a % b))); break; }
                case 0x83: { int64_t b = pop().i64, a = pop().i64; push(Value::I64(a & b)); break; }
                case 0x84: { int64_t b = pop().i64, a = pop().i64; push(Value::I64(a | b)); break; }
                case 0x85: { int64_t b = pop().i64, a = pop().i64; push(Value::I64(a ^ b)); break; }
                case 0x86: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(a << (b & 63)))); break; }
                case 0x87: { int64_t b = pop().i64, a = pop().i64; push(Value::I64(a >> (b & 63))); break; }
                case 0x88: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(a >> (b & 63)))); break; }
                case 0x89: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(std::rotl(a, static_cast<int>(b & 63))))); break; }
                case 0x8A: { uint64_t b = static_cast<uint64_t>(pop().i64), a = static_cast<uint64_t>(pop().i64); push(Value::I64(std::bit_cast<int64_t>(std::rotr(a, static_cast<int>(b & 63))))); break; }
                // f32 arithmetic
                case 0x8B: push(Value::F32(std::fabs(pop().f32))); break;
                case 0x8C: push(Value::F32(-pop().f32)); break;
                case 0x8D: push(Value::F32(std::ceil(pop().f32))); break;
                case 0x8E: push(Value::F32(std::floor(pop().f32))); break;
                case 0x8F: push(Value::F32(std::trunc(pop().f32))); break;
                case 0x90: push(Value::F32(std::nearbyint(pop().f32))); break;
                case 0x91: push(Value::F32(std::sqrt(pop().f32))); break;
                case 0x92: { float b = pop().f32, a = pop().f32; push(Value::F32(a + b)); break; }
                case 0x93: { float b = pop().f32, a = pop().f32; push(Value::F32(a - b)); break; }
                case 0x94: { float b = pop().f32, a = pop().f32; push(Value::F32(a * b)); break; }
                case 0x95: { float b = pop().f32, a = pop().f32; push(Value::F32(a / b)); break; }
                case 0x96: { float b = pop().f32, a = pop().f32; push(Value::F32(wasm_min(a, b))); break; }
                case 0x97: { float b = pop().f32, a = pop().f32; push(Value::F32(wasm_max(a, b))); break; }
                case 0x98: { float b = pop().f32, a = pop().f32; push(Value::F32(std::copysign(a, b))); break; }
                // f64 arithmetic
                case 0x99: push(Value::F64(std::fabs(pop().f64))); break;
                case 0x9A: push(Value::F64(-pop().f64)); break;
                case 0x9B: push(Value::F64(std::ceil(pop().f64))); break;
                case 0x9C: push(Value::F64(std::floor(pop().f64))); break;
                case 0x9D: push(Value::F64(std::trunc(pop().f64))); break;
                case 0x9E: push(Value::F64(std::nearbyint(pop().f64))); break;
                case 0x9F: push(Value::F64(std::sqrt(pop().f64))); break;
                case 0xA0: { double b = pop().f64, a = pop().f64; push(Value::F64(a + b)); break; }
                case 0xA1: { double b = pop().f64, a = pop().f64; push(Value::F64(a - b)); break; }
                case 0xA2: { double b = pop().f64, a = pop().f64; push(Value::F64(a * b)); break; }
                case 0xA3: { double b = pop().f64, a = pop().f64; push(Value::F64(a / b)); break; }
                case 0xA4: { double b = pop().f64, a = pop().f64; push(Value::F64(wasm_min(a, b))); break; }
                case 0xA5: { double b = pop().f64, a = pop().f64; push(Value::F64(wasm_max(a, b))); break; }
                case 0xA6: { double b = pop().f64, a = pop().f64; push(Value::F64(std::copysign(a, b))); break; }
                // conversions and reinterpretations
                case 0xA7: push(Value::I32(static_cast<int32_t>(pop().i64))); break;
                case 0xA8: { int32_t out; if (!trunc_signed(pop().f32, out)) return sig; push(Value::I32(out)); break; }
                case 0xA9: { uint32_t out; if (!trunc_unsigned(pop().f32, out)) return sig; push(Value::I32(std::bit_cast<int32_t>(out))); break; }
                case 0xAA: { int32_t out; if (!trunc_signed(pop().f64, out)) return sig; push(Value::I32(out)); break; }
                case 0xAB: { uint32_t out; if (!trunc_unsigned(pop().f64, out)) return sig; push(Value::I32(std::bit_cast<int32_t>(out))); break; }
                case 0xAC: push(Value::I64(static_cast<int64_t>(popi()))); break;
                case 0xAD: push(Value::I64(static_cast<uint32_t>(popi()))); break;
                case 0xAE: { int64_t out; if (!trunc_signed(pop().f32, out)) return sig; push(Value::I64(out)); break; }
                case 0xAF: { uint64_t out; if (!trunc_unsigned(pop().f32, out)) return sig; push(Value::I64(std::bit_cast<int64_t>(out))); break; }
                case 0xB0: { int64_t out; if (!trunc_signed(pop().f64, out)) return sig; push(Value::I64(out)); break; }
                case 0xB1: { uint64_t out; if (!trunc_unsigned(pop().f64, out)) return sig; push(Value::I64(std::bit_cast<int64_t>(out))); break; }
                case 0xB2: push(Value::F32(static_cast<float>(popi()))); break;
                case 0xB3: push(Value::F32(static_cast<float>(static_cast<uint32_t>(popi())))); break;
                case 0xB4: push(Value::F32(static_cast<float>(pop().i64))); break;
                case 0xB5: push(Value::F32(static_cast<float>(static_cast<uint64_t>(pop().i64)))); break;
                case 0xB6: push(Value::F32(static_cast<float>(pop().f64))); break;
                case 0xB7: push(Value::F64(static_cast<double>(popi()))); break;
                case 0xB8: push(Value::F64(static_cast<double>(static_cast<uint32_t>(popi())))); break;
                case 0xB9: push(Value::F64(static_cast<double>(pop().i64))); break;
                case 0xBA: push(Value::F64(static_cast<double>(static_cast<uint64_t>(pop().i64)))); break;
                case 0xBB: push(Value::F64(static_cast<double>(pop().f32))); break;
                case 0xBC: push(Value::I32(std::bit_cast<int32_t>(pop().f32))); break;
                case 0xBD: push(Value::I64(std::bit_cast<int64_t>(pop().f64))); break;
                case 0xBE: push(Value::F32(std::bit_cast<float>(pop().i32))); break;
                case 0xBF: push(Value::F64(std::bit_cast<double>(pop().i64))); break;
                // sign-extension operators
                case 0xC0: push(Value::I32(static_cast<int32_t>(std::bit_cast<int8_t>(static_cast<uint8_t>(popi()))))); break;
                case 0xC1: push(Value::I32(static_cast<int32_t>(std::bit_cast<int16_t>(static_cast<uint16_t>(popi()))))); break;
                case 0xC2: push(Value::I64(static_cast<int64_t>(std::bit_cast<int8_t>(static_cast<uint8_t>(pop().i64))))); break;
                case 0xC3: push(Value::I64(static_cast<int64_t>(std::bit_cast<int16_t>(static_cast<uint16_t>(pop().i64))))); break;
                case 0xC4: push(Value::I64(static_cast<int64_t>(std::bit_cast<int32_t>(static_cast<uint32_t>(pop().i64))))); break;
                default:
                    fail("unsupported opcode 0x" + std::to_string((int)op));
                    return sig;
            }
        }
        pc = r.p - code.data();
        return sig;
    }

    void read_blocktype(Reader& r) {
        // blocktype: 0x40 (void), a valtype byte, or a (positive) sleb type index.
        uint8_t b = r.u8();
        if (b == 0x40 || b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C) return;
        // otherwise it was a (single-byte here) type index; already consumed.
    }

    // Find the position just past the matching `end` for a body starting at `from`.
    size_t skip_to_end(const std::vector<uint8_t>& code, size_t from) {
        size_t pos = find_else_or_end(code, from);
        // advance to the actual matching end (skip an else section)
        while (pos < code.size() && code[pos] == 0x05) {
            pos = find_else_or_end(code, pos + 1);
        }
        return pos < code.size() ? pos + 1 : code.size();
    }
    // Returns the index of the matching `end`(0x0B) or top-level `else`(0x05).
    size_t find_else_or_end(const std::vector<uint8_t>& code, size_t from) {
        Reader r{code.data() + from, code.data() + code.size()};
        int depth = 0;
        while (!r.eof()) {
            const uint8_t* op_at = r.p;
            uint8_t op = r.u8();
            if (op == 0x02 || op == 0x03 || op == 0x04) { read_blocktype(r); depth++; continue; }
            if (op == 0x0B) { if (depth == 0) return op_at - code.data(); depth--; continue; }
            if (op == 0x05) { if (depth == 0) return op_at - code.data(); continue; }
            skip_immediates(r, op);
        }
        return code.size();
    }
    // Consume the immediate operands of an opcode (so bracket-matching is exact).
    void skip_immediates(Reader& r, uint8_t op) {
        switch (op) {
            case 0x0E: {
                uint32_t count =
                    static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i <= count; ++i)
                    r.uleb();
                break;
            }
            case 0x0C: case 0x0D: case 0x10: case 0x20: case 0x21: case 0x22:
            case 0x23: case 0x24: case 0x25: case 0x26:
            case 0x41: case 0xD2: r.sleb(); break;
            case 0x42: r.sleb(); break;
            case 0x43: r.f32(); break;
            case 0x44: r.f64(); break;
            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            case 0x30: case 0x31: case 0x32: case 0x33:
            case 0x34: case 0x35: case 0x36: case 0x37:
            case 0x38: case 0x39: case 0x3A: case 0x3B:
            case 0x3C: case 0x3D: case 0x3E:
                r.uleb(); r.uleb(); break;
            case 0x3F: case 0x40: r.u8(); break;
            case 0x11: r.uleb(); r.uleb(); break;  // call_indirect
            case 0xD0: r.u8(); break;
            case 0xFC: {
                uint32_t subopcode =
                    static_cast<uint32_t>(r.uleb());
                switch (subopcode) {
                    case 8: case 10: case 12: case 14:
                        r.uleb(); r.uleb(); break;
                    case 9: case 11: case 13:
                    case 15: case 16: case 17:
                        r.uleb(); break;
                    default: break;
                }
                break;
            }
            default: break;  // no immediates
        }
    }

    uint32_t memory_offset(Reader& r) {
        r.uleb();  // alignment is a validation hint, not part of the address.
        return static_cast<uint32_t>(r.uleb());
    }

    uint64_t effective_address(int32_t dynamic, uint32_t offset) {
        return static_cast<uint64_t>(static_cast<uint32_t>(dynamic)) + offset;
    }

    uint64_t memory_address(Reader& r) {
        uint32_t offset = memory_offset(r);
        return effective_address(pop().i32, offset);
    }

    bool same_type(const FuncType& a, const FuncType& b) const {
        return a.params == b.params && a.results == b.results;
    }

    template <typename Int, typename Float>
    bool trunc_signed(Float value, Int& out) {
        if (!std::isfinite(value)) {
            fail("invalid conversion to integer");
            return false;
        }
        long double truncated = std::trunc(
            static_cast<long double>(value));
        if (truncated <
                static_cast<long double>(
                    std::numeric_limits<Int>::min()) ||
            truncated >
                static_cast<long double>(
                    std::numeric_limits<Int>::max())) {
            fail("integer overflow");
            return false;
        }
        out = static_cast<Int>(truncated);
        return true;
    }

    template <typename UInt, typename Float>
    bool trunc_unsigned(Float value, UInt& out) {
        if (!std::isfinite(value)) {
            fail("invalid conversion to integer");
            return false;
        }
        long double truncated = std::trunc(
            static_cast<long double>(value));
        if (truncated < 0 ||
            truncated >
                static_cast<long double>(
                    std::numeric_limits<UInt>::max())) {
            fail("integer overflow");
            return false;
        }
        out = static_cast<UInt>(truncated);
        return true;
    }

    template <typename Float>
    Float wasm_min(Float a, Float b) {
        if (std::isnan(a) || std::isnan(b))
            return std::numeric_limits<Float>::quiet_NaN();
        if (a == 0 && b == 0)
            return std::signbit(a) || std::signbit(b)
                ? -static_cast<Float>(0)
                : static_cast<Float>(0);
        return std::min(a, b);
    }

    template <typename Float>
    Float wasm_max(Float a, Float b) {
        if (std::isnan(a) || std::isnan(b))
            return std::numeric_limits<Float>::quiet_NaN();
        if (a == 0 && b == 0)
            return std::signbit(a) && std::signbit(b)
                ? -static_cast<Float>(0)
                : static_cast<Float>(0);
        return std::max(a, b);
    }

    template <typename T>
    using MemBits = std::conditional_t<sizeof(T) == 1, uint8_t,
                    std::conditional_t<sizeof(T) == 2, uint16_t,
                    std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>>;

    template <typename T>
    T load_mem(uint64_t addr) {
        if (addr > mem().data.size() ||
            sizeof(T) > mem().data.size() - static_cast<size_t>(addr)) {
            fail("out of bounds memory access");
            return T{};
        }
        MemBits<T> bits = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            bits |= static_cast<MemBits<T>>(mem().data[addr + i]) << (i * 8);
        if constexpr (std::is_same_v<T, MemBits<T>>)
            return bits;
        else
            return std::bit_cast<T>(bits);
    }

    template <typename T>
    void store_mem(uint64_t addr, T value) {
        if (addr > mem().data.size() ||
            sizeof(T) > mem().data.size() - static_cast<size_t>(addr)) {
            fail("out of bounds memory access");
            return;
        }
        MemBits<T> bits;
        if constexpr (std::is_same_v<T, MemBits<T>>)
            bits = value;
        else
            bits = std::bit_cast<MemBits<T>>(value);
        for (size_t i = 0; i < sizeof(T); ++i)
            mem().data[addr + i] =
                static_cast<uint8_t>(bits >> (i * 8));
    }

    // Invoke function #fi using values already on the stack as its arguments.
    void call_into(uint32_t fi) {
        if (fi >= inst.funcs_.size()) { fail("bad func index"); return; }
        call_stack.push_back(fi);
        struct CallStackGuard {
            std::vector<uint32_t>& stack;
            ~CallStackGuard() { stack.pop_back(); }
        } guard{call_stack};
        Func& f = inst.funcs_[fi];
        const FuncType& ft = mod.types[f.type_index];
        size_t np = ft.params.size();
        if (stack.size() < np) { fail("call: stack underflow"); return; }
        std::vector<Value> args(stack.end() - np, stack.end());
        stack.resize(stack.size() - np);
        if (f.is_import) {
            std::vector<Value> rets = f.host ? f.host(args) : std::vector<Value>{};
            std::string entry = "#" + std::to_string(fi);
            if (fi < mod.func_imports.size())
                entry += " " + mod.func_imports[fi].first + "." +
                         mod.func_imports[fi].second;
            entry += "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) entry += ",";
                entry += value_debug(args[i]);
            }
            entry += ")->";
            if (rets.empty()) {
                entry += "void";
            } else {
                for (size_t i = 0; i < rets.size(); ++i) {
                    if (i) entry += ",";
                    entry += value_debug(rets[i]);
                }
            }
            host_history.push_back(std::move(entry));
            if (host_history.size() > 8)
                host_history.erase(host_history.begin());
            for (auto& v : rets) push(v);
            return;
        }
        // Set up locals: params + declared locals (zeroed).
        std::vector<Value> locals = args;
        for (ValType t : f.locals) { Value z; z.type = t; z.i64 = 0; locals.push_back(z); }
        size_t saved = stack.size();
        size_t pc = 0;
        Signal s = run(f.code, pc, locals, 0);
        (void)s;
        // Results are the top `nr` values; trim anything else this frame left.
        size_t nr = ft.results.size();
        if (stack.size() < saved + nr) {
            // function fell through producing fewer values than declared; pad.
            while (stack.size() < saved + nr) push(Value::I32(0));
        }
        std::vector<Value> rets(stack.end() - nr, stack.end());
        stack.resize(saved);
        for (auto& v : rets) push(v);
    }

    std::string value_debug(const Value& value) const {
        switch (value.type) {
            case ValType::I32:
                return "i32:" + std::to_string(value.i32);
            case ValType::I64:
                return "i64:" + std::to_string(value.i64);
            case ValType::F32:
                return "f32:" + std::to_string(value.f32);
            case ValType::F64:
                return "f64:" + std::to_string(value.f64);
            case ValType::ExternRef:
                return "externref:" + std::to_string(value.ref);
            case ValType::FuncRef:
                return "funcref:" + std::to_string(value.ref);
            case ValType::Void:
                return "void";
        }
        return "?";
    }
};

int Instance::export_func(const std::string& name) const {
    for (const auto& e : module_->exports)
        if (e.kind == 0 && e.name == name) return static_cast<int>(e.index);
    return -1;
}

std::optional<std::vector<Value>> Instance::invoke(uint32_t func_index,
                                                   const std::vector<Value>& args,
                                                   std::string& error) {
    if (func_index >= funcs_.size()) { error = "bad function index"; return std::nullopt; }
    Interp it{*this, *module_, error};
    for (auto& a : args) it.push(a);
    it.call_into(func_index);
    if (it.failed) return std::nullopt;
    const FuncType& ft = module_->types[funcs_[func_index].type_index];
    size_t nr = ft.results.size();
    std::vector<Value> rets;
    if (it.stack.size() >= nr) rets.assign(it.stack.end() - nr, it.stack.end());
    return rets;
}

std::unique_ptr<Instance> instantiate(const Module& m, const std::vector<HostFn>& host_funcs,
                                      std::string& error) {
    auto inst = std::make_unique<Instance>(&m);
    inst->funcs_ = m.funcs;  // copy (so we can bind host fns)
    inst->globals_ = m.globals;
    if (m.has_memory) {
        inst->memory_.data.assign(
            static_cast<size_t>(m.mem_min_pages) * Memory::kPageSize, 0);
        inst->memory_.max_pages = m.mem_max_pages;
    }
    inst->tables_.reserve(m.tables.size());
    inst->data_dropped_.assign(m.data_segments.size(), false);
    inst->element_dropped_.assign(m.element_segments.size(), false);
    for (const Table& table : m.tables) {
        TableStorage storage;
        storage.element_type = table.element_type;
        storage.max_size = table.max_size;
        storage.has_max = table.has_max;
        storage.elements.assign(
            table.min_size,
            Value::Ref(val_type(table.element_type), 0));
        inst->tables_.push_back(std::move(storage));
    }
    for (const ElementSegment& segment : m.element_segments) {
        if (segment.mode != ElementSegment::Mode::Active) continue;
        if (segment.table_index >= inst->tables_.size()) {
            error = "active element segment targets unavailable table";
            return nullptr;
        }
        int32_t signed_offset = segment.constant_offset;
        if (segment.offset_global) {
            if (*segment.offset_global >= inst->globals_.size() ||
                inst->globals_[*segment.offset_global].value.type !=
                    ValType::I32) {
                error = "element segment offset global is unavailable";
                return nullptr;
            }
            signed_offset =
                inst->globals_[*segment.offset_global].value.i32;
        }
        uint64_t offset =
            static_cast<uint64_t>(static_cast<uint32_t>(signed_offset));
        TableStorage& table = inst->tables_[segment.table_index];
        if (offset > table.elements.size() ||
            segment.function_indices.size() >
                table.elements.size() -
                    static_cast<size_t>(offset)) {
            error = "active element segment is out of bounds";
            return nullptr;
        }
        for (size_t i = 0; i < segment.function_indices.size(); ++i) {
            const auto& function = segment.function_indices[i];
            if (function && *function >= inst->funcs_.size()) {
                error = "element segment function is out of range";
                return nullptr;
            }
            table.elements[static_cast<size_t>(offset) + i] =
                Value::Ref(
                    ValType::FuncRef,
                    function ? static_cast<uint64_t>(*function) + 1 : 0);
        }
    }
    for (const DataSegment& segment : m.data_segments) {
        if (segment.passive) continue;
        if (!m.has_memory || segment.memory_index != 0) {
            error = "active data segment targets unavailable memory";
            return nullptr;
        }
        int32_t signed_offset = segment.constant_offset;
        if (segment.offset_global) {
            if (*segment.offset_global >= inst->globals_.size() ||
                inst->globals_[*segment.offset_global].value.type !=
                    ValType::I32) {
                error = "data segment offset global is unavailable";
                return nullptr;
            }
            signed_offset =
                inst->globals_[*segment.offset_global].value.i32;
        }
        uint64_t offset =
            static_cast<uint64_t>(static_cast<uint32_t>(signed_offset));
        if (offset > inst->memory_.data.size() ||
            segment.bytes.size() >
                inst->memory_.data.size() - static_cast<size_t>(offset)) {
            error = "active data segment is out of bounds";
            return nullptr;
        }
        std::copy(segment.bytes.begin(), segment.bytes.end(),
                  inst->memory_.data.begin() +
                      static_cast<size_t>(offset));
    }

    // Bind imported host functions in order.
    size_t hi = 0;
    for (auto& f : inst->funcs_) {
        if (f.is_import) {
            if (hi < host_funcs.size()) f.host = host_funcs[hi];
            hi++;
        }
    }
    if (hi != host_funcs.size() && !host_funcs.empty()) {
        // tolerated: extra host fns ignored
    }
    if (m.start_func >= 0) {
        std::string e;
        inst->invoke(static_cast<uint32_t>(m.start_func), {}, e);
        if (!e.empty()) { error = "start: " + e; return nullptr; }
    }
    return inst;
}

} // namespace malibu::wasm
