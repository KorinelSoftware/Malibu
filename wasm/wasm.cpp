// wasm/wasm.cpp
// MalibuWASM: binary decoder + structured stack-machine interpreter.

#include "malibu/wasm/wasm.h"

#include <cstring>

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
        default:   return ValType::I32;
    }
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
                        r.u8(); uint8_t flags = r.u8(); r.uleb(); if (flags) r.uleb();
                    } else if (kind == 2) {  // memory
                        uint8_t flags = r.u8(); r.uleb(); if (flags) r.uleb();
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
            case 5: {  // memory
                uint32_t n = static_cast<uint32_t>(r.uleb());
                for (uint32_t i = 0; i < n; ++i) {
                    uint8_t flags = r.u8();
                    m->mem_min_pages = static_cast<uint32_t>(r.uleb());
                    if (flags) m->mem_max_pages = static_cast<uint32_t>(r.uleb());
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
                case 0x00: fail("unreachable"); return sig;                // unreachable
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
                case 0x0F: sig.kind = Signal::Return; return sig;          // return
                case 0x10: {                                               // call
                    uint32_t fi = (uint32_t)r.uleb();
                    call_into(fi);
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
                // constants
                case 0x41: push(Value::I32((int32_t)r.sleb())); break;
                case 0x42: push(Value::I64(r.sleb())); break;
                case 0x43: push(Value::F32(r.f32())); break;
                case 0x44: push(Value::F64(r.f64())); break;
                // memory load/store (mem flags + offset)
                case 0x28: { r.uleb(); uint32_t off = (uint32_t)r.uleb(); push(Value::I32(load32((uint32_t)popi() + off))); break; }  // i32.load
                case 0x2D: { r.uleb(); uint32_t off = (uint32_t)r.uleb(); push(Value::I32(load8u((uint32_t)popi() + off))); break; } // i32.load8_u
                case 0x2C: { r.uleb(); uint32_t off = (uint32_t)r.uleb(); push(Value::I32((int8_t)load8u((uint32_t)popi() + off))); break; } // i32.load8_s
                case 0x36: { r.uleb(); uint32_t off = (uint32_t)r.uleb(); int32_t v = popi(); store32((uint32_t)popi() + off, v); break; } // i32.store
                case 0x3A: { r.uleb(); uint32_t off = (uint32_t)r.uleb(); int32_t v = popi(); store8((uint32_t)popi() + off, (uint8_t)v); break; } // i32.store8
                case 0x3F: r.u8(); push(Value::I32((int32_t)(mem().data.size() / Memory::kPageSize))); break;  // memory.size
                case 0x40: { r.u8(); int32_t delta = popi(); int32_t old = (int32_t)(mem().data.size() / Memory::kPageSize); mem().data.resize(mem().data.size() + (size_t)delta * Memory::kPageSize, 0); push(Value::I32(old)); break; }  // memory.grow
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
                // i32 arithmetic
                case 0x6A: { int32_t b = popi(), a = popi(); push(Value::I32(a + b)); break; }
                case 0x6B: { int32_t b = popi(), a = popi(); push(Value::I32(a - b)); break; }
                case 0x6C: { int32_t b = popi(), a = popi(); push(Value::I32(a * b)); break; }
                case 0x6D: { int32_t b = popi(), a = popi(); if (b == 0) { fail("div by zero"); return sig; } push(Value::I32(a / b)); break; }  // div_s
                case 0x6E: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); if (b == 0) { fail("div by zero"); return sig; } push(Value::I32((int32_t)(a / b))); break; }  // div_u
                case 0x6F: { int32_t b = popi(), a = popi(); if (b == 0) { fail("rem by zero"); return sig; } push(Value::I32(b == -1 ? 0 : a % b)); break; }  // rem_s
                case 0x70: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); if (b == 0) { fail("rem by zero"); return sig; } push(Value::I32((int32_t)(a % b))); break; }  // rem_u
                case 0x71: { int32_t b = popi(), a = popi(); push(Value::I32(a & b)); break; }
                case 0x72: { int32_t b = popi(), a = popi(); push(Value::I32(a | b)); break; }
                case 0x73: { int32_t b = popi(), a = popi(); push(Value::I32(a ^ b)); break; }
                case 0x74: { int32_t b = popi(), a = popi(); push(Value::I32(a << (b & 31))); break; }   // shl
                case 0x75: { int32_t b = popi(), a = popi(); push(Value::I32(a >> (b & 31))); break; }   // shr_s
                case 0x76: { uint32_t b = (uint32_t)popi(), a = (uint32_t)popi(); push(Value::I32((int32_t)(a >> (b & 31)))); break; }  // shr_u
                // f64 arithmetic (common subset)
                case 0xA0: { double b = pop().f64, a = pop().f64; push(Value::F64(a + b)); break; }
                case 0xA1: { double b = pop().f64, a = pop().f64; push(Value::F64(a - b)); break; }
                case 0xA2: { double b = pop().f64, a = pop().f64; push(Value::F64(a * b)); break; }
                case 0xA3: { double b = pop().f64, a = pop().f64; push(Value::F64(a / b)); break; }
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
            case 0x0C: case 0x0D: case 0x10: case 0x20: case 0x21: case 0x22:
            case 0x23: case 0x24: case 0x41: r.sleb(); break;
            case 0x42: r.sleb(); break;
            case 0x43: r.f32(); break;
            case 0x44: r.f64(); break;
            case 0x28: case 0x2C: case 0x2D: case 0x36: case 0x3A: r.uleb(); r.uleb(); break;
            case 0x3F: case 0x40: r.u8(); break;
            case 0x11: r.uleb(); r.uleb(); break;  // call_indirect
            default: break;  // no immediates
        }
    }

    int32_t load32(uint32_t addr) { if (addr + 4 > mem().data.size()) { fail("oob load"); return 0; } int32_t v; std::memcpy(&v, mem().data.data() + addr, 4); return v; }
    uint8_t load8u(uint32_t addr) { if (addr >= mem().data.size()) { fail("oob load"); return 0; } return mem().data[addr]; }
    void store32(uint32_t addr, int32_t v) { if (addr + 4 > mem().data.size()) { fail("oob store"); return; } std::memcpy(mem().data.data() + addr, &v, 4); }
    void store8(uint32_t addr, uint8_t v) { if (addr >= mem().data.size()) { fail("oob store"); return; } mem().data[addr] = v; }

    // Invoke function #fi using values already on the stack as its arguments.
    void call_into(uint32_t fi) {
        if (fi >= inst.funcs_.size()) { fail("bad func index"); return; }
        Func& f = inst.funcs_[fi];
        const FuncType& ft = mod.types[f.type_index];
        size_t np = ft.params.size();
        if (stack.size() < np) { fail("call: stack underflow"); return; }
        std::vector<Value> args(stack.end() - np, stack.end());
        stack.resize(stack.size() - np);
        if (f.is_import) {
            std::vector<Value> rets = f.host ? f.host(args) : std::vector<Value>{};
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
    if (m.has_memory) inst->memory_.data.assign(static_cast<size_t>(m.mem_min_pages) * Memory::kPageSize, 0);

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
