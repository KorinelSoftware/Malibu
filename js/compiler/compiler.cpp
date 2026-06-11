// js/compiler/compiler.cpp
// Recursive AST -> bytecode compiler. Registers are scratch for expression
// evaluation; named bindings live in lexical Environments at run time.

#include "malibu/js/compiler/compiler.h"
#include "malibu/js/bytecode/bytecode.h"

#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

namespace malibu::js::compiler {
namespace {

using parser::Node;
using parser::NodeKind;
using bytecode::OpCode;
using bytecode::encode;

std::u16string u16(const std::string& s) { return std::u16string(s.begin(), s.end()); }

double parse_number_literal(const std::string& raw) {
    std::string s;
    for (char c : raw) if (c != '_') s.push_back(c);
    try {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            return static_cast<double>(std::stoll(s.substr(2), nullptr, 16));
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
            return static_cast<double>(std::stoll(s.substr(2), nullptr, 2));
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O'))
            return static_cast<double>(std::stoll(s.substr(2), nullptr, 8));
        return std::stod(s);
    } catch (...) { return 0.0; }
}

// Thrown internally to abort compilation with a message.
struct CompileError { std::string message; };

struct LoopCtx {
    std::vector<int> break_jumps;
    std::vector<int> continue_jumps;
    bool             is_switch = false;  // break targets it, continue passes through
    std::string      label;              // for labeled break/continue
};

struct FnCtx {
    Function*            fn;
    int                  reg_top = 0;
    bool                 is_async = false;
    std::vector<LoopCtx> loops;
};

class Impl {
public:
    Compiler::Result run(const Node& program) {
        Compiler::Result result;
        try {
            result.function = compile_function_node(program, /*isProgram*/true, u"<main>");
        } catch (const CompileError& e) {
            result.function = nullptr;
            result.error = e.message;
        }
        return result;
    }

private:
    FnCtx* cur_ = nullptr;

    // ---- emit / pools / registers ----
    int emit(OpCode op, uint8_t dst, uint16_t a, uint16_t b, int16_t imm) {
        cur_->fn->code.push_back(encode(op, dst, a, b, imm));
        return static_cast<int>(cur_->fn->code.size()) - 1;
    }
    void patch_imm(int idx, int target) {
        auto in = bytecode::decode(cur_->fn->code[idx]);
        cur_->fn->code[idx] = encode(in.op, in.dst, in.src_a, in.src_b,
                                     static_cast<int16_t>(target));
    }
    int here() const { return static_cast<int>(cur_->fn->code.size()); }

    int str_const(const std::u16string& s) {
        auto& pool = cur_->fn->str_consts;
        for (size_t i = 0; i < pool.size(); ++i) if (pool[i] == s) return static_cast<int>(i);
        pool.push_back(s);
        return static_cast<int>(pool.size()) - 1;
    }
    int num_const(vm::Value v) {
        cur_->fn->num_consts.push_back(v);
        return static_cast<int>(cur_->fn->num_consts.size()) - 1;
    }
    uint8_t alloc() {
        int r = cur_->reg_top++;
        if (cur_->reg_top > static_cast<int>(cur_->fn->num_registers))
            cur_->fn->num_registers = static_cast<uint32_t>(cur_->reg_top);
        if (r > 255) throw CompileError{"expression too complex (register overflow)"};
        return static_cast<uint8_t>(r);
    }
    int alloc_block(int n) {
        int base = cur_->reg_top;
        cur_->reg_top += n;
        if (cur_->reg_top > static_cast<int>(cur_->fn->num_registers))
            cur_->fn->num_registers = static_cast<uint32_t>(cur_->reg_top);
        if (cur_->reg_top > 256) throw CompileError{"call has too many arguments"};
        return base;
    }
    void free_to(int mark) { cur_->reg_top = mark; }

    std::string pending_label_;  // label attached to the next loop/switch, if any
    void push_loop(bool is_switch = false) {
        cur_->loops.push_back(LoopCtx{});
        cur_->loops.back().is_switch = is_switch;
        cur_->loops.back().label = pending_label_;
        pending_label_.clear();
    }
    void compile_labeled(const Node& n) {
        if (n.children.empty()) return;
        const Node& inner = *n.children[0];
        NodeKind k = inner.kind;
        if (k == NodeKind::For || k == NodeKind::While || k == NodeKind::DoWhile ||
            k == NodeKind::Switch) {
            pending_label_ = n.str;   // consumed by the loop/switch when it pushes
            compile_stmt(inner);
            pending_label_.clear();
        } else {
            // Labeled block (or any other statement): a break-only target.
            cur_->loops.push_back(LoopCtx{});
            cur_->loops.back().is_switch = true;     // break-only; continue passes through
            cur_->loops.back().label = n.str;
            compile_stmt(inner);
            LoopCtx lc = std::move(cur_->loops.back());
            cur_->loops.pop_back();
            for (int j : lc.break_jumps) patch_imm(j, here());
        }
    }

    // ---- function compilation ----
    std::shared_ptr<Function> compile_function_node(const Node& node, bool isProgram,
                                                    const std::u16string& name) {
        auto fn = std::make_shared<Function>();
        fn->name = name;
        fn->is_arrow = node.kind == NodeKind::ArrowFunction;
        fn->is_async = !isProgram && node.is_async;
        fn->is_generator = !isProgram && (node.flags & parser::node_flags::Generator);

        FnCtx ctx; ctx.fn = fn.get();
        ctx.is_async = fn->is_async;
        FnCtx* saved = cur_;
        cur_ = &ctx;

        const Node* body = nullptr;
        // Parameters that need a runtime prologue: {node, slot-name, rest-index}.
        // rest_index >= 0 marks a rest parameter (bound from `arguments`).
        struct PrologueParam { const Node* node; std::u16string slot; int rest_index; };
        std::vector<PrologueParam> prologue_params;
        if (isProgram) {
            body = &node;
        } else {
            // params = all children except last; last = body
            size_t nparams = node.children.empty() ? 0 : node.children.size() - 1;
            uint32_t simple_leading = 0;
            bool seen_complex = false;
            for (size_t i = 0; i < nparams; ++i) {
                const Node* p = node.children[i].get();
                if (p->kind == NodeKind::Spread) {
                    // Rest parameter: no named slot; bound from `arguments`.
                    prologue_params.push_back({p->children.empty() ? p : p->children[0].get(),
                                               u"", static_cast<int>(fn->param_names.size())});
                    seen_complex = true;
                } else if (p->kind == NodeKind::Identifier && p->children.empty()) {
                    fn->param_names.push_back(u16(p->str));
                    if (!seen_complex) ++simple_leading;
                } else {
                    // Default and/or destructuring pattern: bind a synthetic slot,
                    // then unpack it at the top of the body.
                    std::u16string slot = u16("%arg" + std::to_string(i) + "%");
                    fn->param_names.push_back(slot);
                    prologue_params.push_back({p, slot, -1});
                    seen_complex = true;
                }
            }
            fn->arity = simple_leading;
            body = node.children.empty() ? nullptr : node.children.back().get();
        }

        auto emit_param_prologue = [&]() {
            for (const auto& pp : prologue_params) {
                int m = cur_->reg_top;
                uint8_t v = alloc();
                if (pp.rest_index >= 0) {
                    uint8_t argsReg = alloc();
                    emit(OpCode::LoadVar, argsReg, 0, 0,
                         static_cast<int16_t>(str_const(u"arguments")));
                    emit_array_slice(v, argsReg, pp.rest_index);
                } else {
                    emit(OpCode::LoadVar, v, 0, 0, static_cast<int16_t>(str_const(pp.slot)));
                }
                bind_pattern_or_target(*pp.node, v, /*declare*/true, /*is_var*/false);
                free_to(m);
            }
        };

        if (isProgram && body) {
            // REPL-style completion value: the program returns the value of its
            // last evaluated top-level expression statement (reg 0 is reserved).
            uint8_t completion = alloc();
            emit(OpCode::LoadUndefined, completion, 0, 0, 0);
            hoist(*body);
            for (auto& stmt : body->children) {
                if (stmt->kind == NodeKind::ExpressionStatement && !stmt->children.empty()) {
                    compile_expr(*stmt->children[0], completion);
                } else {
                    compile_stmt(*stmt);
                }
            }
            emit(OpCode::Return, 0, completion, 0, 0);
        } else if (body) {
            emit_param_prologue();
            if (body->kind == NodeKind::Block) {
                hoist(*body);
                for (auto& stmt : body->children) compile_stmt(*stmt);
            } else {
                uint8_t r = alloc();  // arrow with expression body
                compile_expr(*body, r);
                emit(OpCode::Return, 0, r, 0, 0);
            }
        } else {
            emit_param_prologue();  // body-less function with destructured params
        }
        emit(OpCode::ReturnUndefined, 0, 0, 0, 0);

        cur_ = saved;
        return fn;
    }

    // Hoist top-level `var` names and function declarations of a body.
    void hoist(const Node& body) {
        for (auto& stmt : body.children) {
            if (stmt->kind == NodeKind::FunctionDeclaration && !stmt->str.empty()) {
                auto nested = compile_function_node(*stmt, false, u16(stmt->str));
                int fidx = static_cast<int>(cur_->fn->functions.size());
                cur_->fn->functions.push_back(nested);
                uint8_t r = alloc();
                emit(OpCode::NewClosure, r, 0, 0, static_cast<int16_t>(fidx));
                emit(OpCode::DefineVar, /*dst=funcScope*/1, r, 0,
                     static_cast<int16_t>(str_const(u16(stmt->str))));
                free_to(cur_->reg_top - 1);
                hoisted_fns_.insert(stmt.get());
            }
        }
        collect_vars(body);
    }
    void collect_vars(const Node& n) {
        for (auto& c : n.children) {
            if (c->kind == NodeKind::FunctionDeclaration || c->kind == NodeKind::ArrowFunction ||
                c->kind == NodeKind::ClassDeclaration)
                continue;  // do not descend into nested functions / class bodies
            if (c->kind == NodeKind::VariableDeclaration && c->str == "var") {
                for (auto& binding : c->children) {
                    uint8_t r = alloc();
                    emit(OpCode::LoadUndefined, r, 0, 0, 0);
                    emit(OpCode::DefineVar, 1, r, 0, static_cast<int16_t>(str_const(u16(binding->str))));
                    free_to(cur_->reg_top - 1);
                }
            }
            collect_vars(*c);
        }
    }

    // ---- statements ----
    void compile_stmt(const Node& n) {
        int mark = cur_->reg_top;
        switch (n.kind) {
            case NodeKind::VariableDeclaration: compile_var_decl(n); break;
            case NodeKind::FunctionDeclaration:
                if (!hoisted_fns_.count(&n)) compile_function_decl_inline(n);
                break;
            case NodeKind::ExpressionStatement:
                if (!n.children.empty()) { uint8_t r = alloc(); compile_expr(*n.children[0], r); }
                break;
            case NodeKind::Block: compile_block(n); break;
            case NodeKind::If: compile_if(n); break;
            case NodeKind::While: compile_while(n); break;
            case NodeKind::With: compile_with(n); break;
            case NodeKind::DoWhile: compile_do_while(n); break;
            case NodeKind::For: compile_for(n); break;
            case NodeKind::Return: compile_return(n); break;
            case NodeKind::Throw: {
                uint8_t r = alloc(); compile_expr(*n.children[0], r);
                emit(OpCode::Throw, 0, r, 0, 0);
                break;
            }
            case NodeKind::Try: compile_try(n); break;
            case NodeKind::Break: {
                LoopCtx* lc = nullptr;
                if (!n.str.empty()) {  // break label
                    for (auto it = cur_->loops.rbegin(); it != cur_->loops.rend(); ++it)
                        if (it->label == n.str) { lc = &*it; break; }
                    if (!lc) throw CompileError{"undefined label '" + n.str + "'"};
                } else {
                    if (cur_->loops.empty()) throw CompileError{"'break' outside loop"};
                    lc = &cur_->loops.back();
                }
                lc->break_jumps.push_back(emit(OpCode::Jump, 0, 0, 0, 0));
                break;
            }
            case NodeKind::Continue: {
                LoopCtx* lc = nullptr;
                if (!n.str.empty()) {  // continue label
                    for (auto it = cur_->loops.rbegin(); it != cur_->loops.rend(); ++it)
                        if (it->label == n.str && !it->is_switch) { lc = &*it; break; }
                    if (!lc) throw CompileError{"undefined label '" + n.str + "'"};
                } else {  // nearest enclosing LOOP, skipping switches/labeled-blocks
                    for (auto it = cur_->loops.rbegin(); it != cur_->loops.rend(); ++it)
                        if (!it->is_switch) { lc = &*it; break; }
                    if (!lc) throw CompileError{"'continue' outside loop"};
                }
                lc->continue_jumps.push_back(emit(OpCode::Jump, 0, 0, 0, 0));
                break;
            }
            case NodeKind::Switch: compile_switch(n); break;
            case NodeKind::Labeled: compile_labeled(n); break;
            case NodeKind::ClassDeclaration: {
                uint8_t r = alloc();
                compile_class(n, r);
                if (!n.str.empty())
                    emit(OpCode::DefineVar, 0, r, 0, static_cast<int16_t>(str_const(u16(n.str))));
                free_to(cur_->reg_top - 1);
                break;
            }
            case NodeKind::ImportDeclaration:
            case NodeKind::ExportDeclaration:
                for (auto& c : n.children) compile_stmt(*c);
                break;
            default: {
                uint8_t r = alloc(); compile_expr(n, r);  // bare expression
                break;
            }
        }
        free_to(mark);
    }

    void compile_block(const Node& n) {
        emit(OpCode::PushScope, 0, 0, 0, 0);
        for (auto& s : n.children) compile_stmt(*s);
        emit(OpCode::PopScope, 0, 0, 0, 0);
    }

    // True when `n` is an anonymous function/arrow/class literal — the cases that
    // receive a name via "named evaluation" (e.g. `let f = function(){}`).
    static bool is_anon_fn(const Node& n) {
        return (n.kind == NodeKind::FunctionDeclaration || n.kind == NodeKind::ArrowFunction ||
                n.kind == NodeKind::ClassDeclaration) && n.str.empty();
    }
    // Compile an initializer into `dst`, applying named evaluation: if the
    // initializer is an anonymous function/class literal, set its `.name`.
    void compile_init_named(const Node& init, uint8_t dst, const std::u16string& name) {
        compile_expr(init, dst);
        if (!name.empty() && is_anon_fn(init))
            emit(OpCode::SetFnName, dst, 0, 0, static_cast<int16_t>(str_const(name)));
    }

    void compile_var_decl(const Node& n) {
        bool is_var = n.str == "var";
        for (auto& binding : n.children) {
            // Destructuring declarator: Assignment(pattern, init).
            if (binding->kind == NodeKind::Assignment) {
                int mark = cur_->reg_top;
                uint8_t src = alloc();
                compile_expr(*binding->children[1], src);
                bind_pattern_or_target(*binding->children[0], src, /*declare*/true, is_var);
                free_to(mark);
                continue;
            }
            uint8_t r = alloc();
            if (!binding->children.empty()) compile_init_named(*binding->children[0], r, u16(binding->str));
            else emit(OpCode::LoadUndefined, r, 0, 0, 0);
            emit(OpCode::DefineVar, is_var ? 1 : 0, r, 0,
                 static_cast<int16_t>(str_const(u16(binding->str))));
            free_to(cur_->reg_top - 1);
        }
    }

    // ---- destructuring -------------------------------------------------------
    // Bind a target (Identifier / nested pattern / Assignment-with-default) from
    // the value currently in `src`. `declare` chooses DefineVar vs assignment.
    void bind_pattern_or_target(const Node& target, uint8_t src, bool declare, bool is_var) {
        if (target.kind == NodeKind::Assignment) {
            // default: if (src === undefined) src = <default>. When the binding is
            // a plain identifier and the default is an anonymous function/class,
            // named evaluation gives it the binding's name.
            const Node& leaf = *target.children[0];
            apply_default(src, *target.children[1],
                          leaf.kind == NodeKind::Identifier ? u16(leaf.str) : std::u16string());
            bind_pattern_or_target(leaf, src, declare, is_var);
            return;
        }
        if (target.kind == NodeKind::ArrayLiteral)  { bind_array_pattern(target, src, declare, is_var); return; }
        if (target.kind == NodeKind::ObjectLiteral) { bind_object_pattern(target, src, declare, is_var); return; }
        // Identifier leaf.
        if (declare)
            emit(OpCode::DefineVar, is_var ? 1 : 0, src, 0,
                 static_cast<int16_t>(str_const(u16(target.str))));
        else
            store_to_target(target, src);
    }

    void apply_default(uint8_t valueReg, const Node& def, const std::u16string& name = {}) {
        int mark = cur_->reg_top;
        uint8_t u = alloc();
        emit(OpCode::LoadUndefined, u, 0, 0, 0);
        uint8_t isU = alloc();
        emit(OpCode::StrictEq, isU, valueReg, u, 0);
        int jskip = emit(OpCode::JumpIfFalse, 0, isU, 0, 0);
        compile_init_named(def, valueReg, name);  // overwrite only when undefined
        patch_imm(jskip, here());
        free_to(mark);
    }

    void bind_array_pattern(const Node& pat, uint8_t srcv, bool declare, bool is_var) {
        // Array destructuring consumes the iteration protocol: materialize the
        // source (array, string, generator, Map/Set, custom iterable) first.
        uint8_t src = alloc();
        emit(OpCode::ToIterable, src, srcv, 0, 0);
        int idx = 0;
        for (auto& el : pat.children) {
            if (el->kind == NodeKind::UndefinedLiteral) { idx++; continue; }  // hole
            if (el->kind == NodeKind::Spread) {
                // rest = src.slice(idx)
                int m = cur_->reg_top;
                uint8_t rest = alloc();
                emit_array_slice(rest, src, idx);
                bind_pattern_or_target(*el->children[0], rest, declare, is_var);
                free_to(m);
                break;
            }
            int m = cur_->reg_top;
            uint8_t ev = alloc();
            uint8_t ir = alloc();
            emit(OpCode::LoadConst, ir, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(idx))));
            emit(OpCode::GetElem, ev, src, ir, 0);
            bind_pattern_or_target(*el, ev, declare, is_var);
            free_to(m);
            idx++;
        }
    }

    void bind_object_pattern(const Node& pat, uint8_t src, bool declare, bool is_var) {
        std::vector<std::u16string> taken;
        for (auto& prop : pat.children) {
            if (prop->kind == NodeKind::Spread) {
                // rest = %CopyRest%(src, ...takenKeys)
                int m = cur_->reg_top;
                int base = alloc_block(2 + static_cast<int>(taken.size()));
                emit(OpCode::LoadVar, static_cast<uint8_t>(base), 0, 0,
                     static_cast<int16_t>(str_const(u"%CopyRest%")));
                emit(OpCode::LoadUndefined, static_cast<uint8_t>(base + 1), 0, 0, 0);
                emit(OpCode::Move, static_cast<uint8_t>(base + 2), src, 0, 0);
                for (size_t i = 0; i < taken.size(); ++i)
                    emit(OpCode::LoadString, static_cast<uint8_t>(base + 3 + i), 0, 0,
                         static_cast<int16_t>(str_const(taken[i])));
                uint8_t rest = alloc();
                emit(OpCode::Call, rest, static_cast<uint16_t>(base), 0,
                     static_cast<int16_t>(1 + taken.size()));
                bind_pattern_or_target(*prop->children[0], rest, declare, is_var);
                free_to(m);
                continue;
            }
            int m = cur_->reg_top;
            uint8_t pv = alloc();
            if (prop->flags & parser::node_flags::Computed) {
                uint8_t kr = alloc();
                compile_expr(*prop->children.back(), kr);
                emit(OpCode::GetElem, pv, src, kr, 0);
            } else {
                taken.push_back(u16(prop->str));
                emit(OpCode::GetProp, pv, src, 0, static_cast<int16_t>(str_const(u16(prop->str))));
            }
            bind_pattern_or_target(*prop->children[0], pv, declare, is_var);
            free_to(m);
        }
    }

    // dst <- src.slice(start) via Array.prototype.slice (existing builtin).
    void emit_array_slice(uint8_t dst, uint8_t src, int start) {
        int base = alloc_block(3);  // [callee][this][startArg]
        emit(OpCode::GetProp, static_cast<uint8_t>(base), src, 0,
             static_cast<int16_t>(str_const(u"slice")));
        emit(OpCode::Move, static_cast<uint8_t>(base + 1), src, 0, 0);
        emit(OpCode::LoadConst, static_cast<uint8_t>(base + 2), 0, 0,
             static_cast<int16_t>(num_const(vm::Value::make_int32(start))));
        emit(OpCode::Call, dst, static_cast<uint16_t>(base), 0, 1);
        cur_->reg_top = base;  // free the call block (dst is below base)
    }

    void compile_function_decl_inline(const Node& n) {
        auto nested = compile_function_node(n, false, u16(n.str));
        int fidx = static_cast<int>(cur_->fn->functions.size());
        cur_->fn->functions.push_back(nested);
        uint8_t r = alloc();
        emit(OpCode::NewClosure, r, 0, 0, static_cast<int16_t>(fidx));
        emit(OpCode::DefineVar, 0, r, 0, static_cast<int16_t>(str_const(u16(n.str))));
    }

    void compile_if(const Node& n) {
        uint8_t c = alloc();
        compile_expr(*n.children[0], c);
        int jfalse = emit(OpCode::JumpIfFalse, 0, c, 0, 0);
        free_to(cur_->reg_top - 1);
        if (n.children.size() > 1) compile_stmt(*n.children[1]);
        if (n.children.size() > 2) {
            int jend = emit(OpCode::Jump, 0, 0, 0, 0);
            patch_imm(jfalse, here());
            compile_stmt(*n.children[2]);
            patch_imm(jend, here());
        } else {
            patch_imm(jfalse, here());
        }
    }

    void compile_while(const Node& n) {
        int start = here();
        uint8_t c = alloc();
        compile_expr(*n.children[0], c);
        int jexit = emit(OpCode::JumpIfFalse, 0, c, 0, 0);
        free_to(cur_->reg_top - 1);
        push_loop();
        if (n.children.size() > 1) compile_stmt(*n.children[1]);
        emit(OpCode::Jump, 0, 0, 0, static_cast<int16_t>(start));
        LoopCtx lc = std::move(cur_->loops.back()); cur_->loops.pop_back();
        patch_imm(jexit, here());
        for (int j : lc.break_jumps) patch_imm(j, here());
        for (int j : lc.continue_jumps) patch_imm(j, start);
    }

    void compile_with(const Node& n) {
        int mark = cur_->reg_top;
        uint8_t obj = alloc();
        compile_expr(*n.children[0], obj);
        emit(OpCode::PushWithScope, 0, obj, 0, 0);
        free_to(mark);
        if (n.children.size() > 1) compile_stmt(*n.children[1]);
        emit(OpCode::PopScope, 0, 0, 0, 0);
    }

    void compile_do_while(const Node& n) {
        int start = here();
        push_loop();
        compile_stmt(*n.children[0]);
        int cont = here();
        uint8_t c = alloc();
        compile_expr(*n.children[1], c);
        emit(OpCode::JumpIfTrue, 0, c, 0, static_cast<int16_t>(start));
        free_to(cur_->reg_top - 1);
        LoopCtx lc = std::move(cur_->loops.back()); cur_->loops.pop_back();
        for (int j : lc.break_jumps) patch_imm(j, here());
        for (int j : lc.continue_jumps) patch_imm(j, cont);
    }

    void compile_for(const Node& n) {
        if (n.str == "of" || n.str == "in") { compile_for_each(n); return; }
        // C-style: children = [init, cond, update, body]
        emit(OpCode::PushScope, 0, 0, 0, 0);
        const Node& init = *n.children[0];
        if (init.kind == NodeKind::VariableDeclaration) compile_var_decl(init);
        else if (init.kind != NodeKind::UndefinedLiteral) { int m = cur_->reg_top; uint8_t r = alloc(); compile_expr(init, r); free_to(m); }

        int start = here();
        int jexit = -1;
        const Node& cond = *n.children[1];
        if (cond.kind != NodeKind::UndefinedLiteral) {
            uint8_t c = alloc();
            compile_expr(cond, c);
            jexit = emit(OpCode::JumpIfFalse, 0, c, 0, 0);
            free_to(cur_->reg_top - 1);
        }
        push_loop();
        compile_stmt(*n.children[3]);
        int cont = here();
        const Node& upd = *n.children[2];
        if (upd.kind != NodeKind::UndefinedLiteral) { int m = cur_->reg_top; uint8_t r = alloc(); compile_expr(upd, r); free_to(m); }
        emit(OpCode::Jump, 0, 0, 0, static_cast<int16_t>(start));
        LoopCtx lc = std::move(cur_->loops.back()); cur_->loops.pop_back();
        if (jexit >= 0) patch_imm(jexit, here());
        for (int j : lc.break_jumps) patch_imm(j, here());
        for (int j : lc.continue_jumps) patch_imm(j, cont);
        emit(OpCode::PopScope, 0, 0, 0, 0);
    }

    void compile_for_each(const Node& n) {
        // children = [binding, iterable, body]; n.str = "of" | "in"
        emit(OpCode::PushScope, 0, 0, 0, 0);
        const Node& binding = *n.children[0];
        // Resolve the loop target: a declared binding (var/let/const, possibly a
        // destructuring pattern) or an existing assignment target.
        const Node* target = nullptr;
        bool declare = false, is_var_decl = false;
        if (binding.kind == NodeKind::VariableDeclaration && !binding.children.empty()) {
            target = binding.children[0].get(); declare = true; is_var_decl = binding.str == "var";
        } else if (binding.kind == NodeKind::Identifier || binding.kind == NodeKind::Member ||
                   binding.kind == NodeKind::ArrayLiteral || binding.kind == NodeKind::ObjectLiteral) {
            target = &binding;
        } else throw CompileError{"unsupported for-" + n.str + " binding"};

        uint8_t r_iter = alloc();
        compile_expr(*n.children[1], r_iter);
        uint8_t r_arr = alloc();
        emit(OpCode::ToIterable, r_arr, r_iter, 0, n.str == "in" ? 1 : 0);
        uint8_t r_len = alloc();
        emit(OpCode::GetLength, r_len, r_arr, 0, 0);
        uint8_t r_i = alloc();
        emit(OpCode::LoadConst, r_i, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(0))));

        int start = here();
        uint8_t r_cmp = alloc();
        emit(OpCode::Lt, r_cmp, r_i, r_len, 0);
        int jexit = emit(OpCode::JumpIfFalse, 0, r_cmp, 0, 0);
        free_to(r_cmp);  // release r_cmp

        uint8_t r_elem = alloc();
        emit(OpCode::GetElem, r_elem, r_arr, r_i, 0);
        emit(OpCode::PushScope, 0, 0, 0, 0);
        bind_pattern_or_target(*target, r_elem, declare, is_var_decl);
        free_to(r_elem);

        push_loop();
        compile_stmt(*n.children[2]);
        emit(OpCode::PopScope, 0, 0, 0, 0);
        int cont = here();
        // r_i = r_i + 1
        uint8_t r_one = alloc();
        emit(OpCode::LoadConst, r_one, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(1))));
        emit(OpCode::Add, r_i, r_i, r_one, 0);
        free_to(r_one);
        emit(OpCode::Jump, 0, 0, 0, static_cast<int16_t>(start));

        LoopCtx lc = std::move(cur_->loops.back()); cur_->loops.pop_back();
        patch_imm(jexit, here());
        for (int j : lc.break_jumps) patch_imm(j, here());
        for (int j : lc.continue_jumps) patch_imm(j, cont);
        emit(OpCode::PopScope, 0, 0, 0, 0);
    }

    // switch (disc) { case t: ... default: ... }
    //   children[0] = discriminant; children[1..] = clauses (Block nodes whose
    //   str is "case" (children[0]=test, rest=stmts) or "default" (children=stmts)).
    void compile_switch(const Node& n) {
        emit(OpCode::PushScope, 0, 0, 0, 0);
        uint8_t rd = alloc();
        compile_expr(*n.children[0], rd);

        // Emit the dispatch: compare disc to each case test; jump to its body.
        std::vector<int> case_jumps;          // parallel to non-default clauses
        std::vector<const Node*> clauses;
        int default_index = -1;
        for (size_t i = 1; i < n.children.size(); ++i) {
            const Node* c = n.children[i].get();
            clauses.push_back(c);
            if (c->str == "default") { default_index = static_cast<int>(clauses.size()) - 1; case_jumps.push_back(-1); continue; }
            int mark = cur_->reg_top;
            uint8_t rt = alloc();
            compile_expr(*c->children[0], rt);
            uint8_t eq = alloc();
            emit(OpCode::StrictEq, eq, rd, rt, 0);
            case_jumps.push_back(emit(OpCode::JumpIfTrue, 0, eq, 0, 0));
            free_to(mark);
        }
        // No case matched: jump to default body (if any) or past the switch.
        int jdefault = emit(OpCode::Jump, 0, 0, 0, 0);

        push_loop(/*is_switch*/true);

        // Emit clause bodies in order (fall-through is natural).
        std::vector<int> body_pcs(clauses.size());
        for (size_t i = 0; i < clauses.size(); ++i) {
            body_pcs[i] = here();
            if (static_cast<int>(case_jumps.size()) > static_cast<int>(i) && case_jumps[i] >= 0)
                patch_imm(case_jumps[i], here());
            const Node* c = clauses[i];
            size_t stmt_start = (c->str == "default") ? 0 : 1;
            for (size_t s = stmt_start; s < c->children.size(); ++s)
                compile_stmt(*c->children[s]);
        }
        int end_pc = here();
        patch_imm(jdefault, default_index >= 0 ? body_pcs[default_index] : end_pc);

        LoopCtx lc = std::move(cur_->loops.back()); cur_->loops.pop_back();
        for (int j : lc.break_jumps) patch_imm(j, end_pc);
        // (switch swallows no continues; they were routed to the enclosing loop)
        emit(OpCode::PopScope, 0, 0, 0, 0);
    }

    void compile_return(const Node& n) {
        if (!n.children.empty()) {
            uint8_t r = alloc();
            compile_expr(*n.children[0], r);
            emit(OpCode::Return, 0, r, 0, 0);
        } else {
            emit(OpCode::ReturnUndefined, 0, 0, 0, 0);
        }
    }

    void compile_try(const Node& n) {
        bool has_catch = n.str.find('c') != std::string::npos;
        bool has_finally = n.str.find('f') != std::string::npos;
        const Node& try_block = *n.children[0];
        const Node* catch_block = has_catch ? n.children[1].get() : nullptr;
        const Node* finally_block = has_finally ? n.children.back().get() : nullptr;

        uint8_t exc_reg = alloc();  // reserved for the in-flight exception
        uint8_t flags = static_cast<uint8_t>((has_catch ? 1 : 0) | (has_finally ? 2 : 0));
        int ph = emit(OpCode::PushHandler, flags, 0, exc_reg, 0);

        compile_stmt(try_block);
        emit(OpCode::PopHandler, 0, 0, 0, 0);
        int jnormal = emit(OpCode::Jump, 0, 0, 0, 0);  // jump to finally/after

        int catch_pc = here();
        if (has_catch) {
            emit(OpCode::PushScope, 0, 0, 0, 0);
            if (!catch_block->str.empty())
                emit(OpCode::DefineVar, 0, exc_reg, 0,
                     static_cast<int16_t>(str_const(u16(catch_block->str))));
            compile_stmt(*catch_block);
            emit(OpCode::PopScope, 0, 0, 0, 0);
        }

        int finally_pc = here();
        patch_imm(jnormal, finally_pc);
        if (has_finally) {
            compile_stmt(*finally_block);
            emit(OpCode::EndFinally, 0, 0, 0, 0);
        }
        // Wire the handler: catch target (or finally if no catch), finally pc.
        {
            auto in = bytecode::decode(cur_->fn->code[ph]);
            int target_catch = has_catch ? catch_pc : finally_pc;
            cur_->fn->code[ph] = encode(in.op, flags,
                                        static_cast<uint16_t>(finally_pc),
                                        exc_reg, static_cast<int16_t>(target_catch));
        }
    }

    // ---- expressions (result written into reg `dst`) ----
    void compile_expr(const Node& n, uint8_t dst) {
        switch (n.kind) {
            case NodeKind::NumberLiteral: {
                double d = parse_number_literal(n.str);
                vm::Value v = (d == std::floor(d) && std::abs(d) < 2147483647.0)
                                  ? vm::Value::make_int32(static_cast<int32_t>(d))
                                  : vm::Value::make_double(d);
                emit(OpCode::LoadConst, dst, 0, 0, static_cast<int16_t>(num_const(v)));
                break;
            }
            case NodeKind::StringLiteral:
            case NodeKind::TemplateLiteral:
                emit(OpCode::LoadString, dst, 0, 0, static_cast<int16_t>(str_const(u16(n.str))));
                break;
            case NodeKind::BoolLiteral:
                emit(OpCode::LoadBool, dst, 0, 0, n.str == "true" ? 1 : 0);
                break;
            case NodeKind::NullLiteral:      emit(OpCode::LoadNull, dst, 0, 0, 0); break;
            case NodeKind::UndefinedLiteral: emit(OpCode::LoadUndefined, dst, 0, 0, 0); break;
            case NodeKind::Identifier: {
                if (n.str == "this") {
                    // Arrows have no own `this`: read the enclosing function's,
                    // captured lexically as %this% (defined by the interpreter).
                    if (cur_->fn->is_arrow)
                        emit(OpCode::LoadVar, dst, 0, 0, static_cast<int16_t>(str_const(u"%this%")));
                    else
                        emit(OpCode::LoadThis, dst, 0, 0, 0);
                    break;
                }
                if (n.str == "undefined") { emit(OpCode::LoadUndefined, dst, 0, 0, 0); break; }
                if (n.str == "super") throw CompileError{"'super' keyword unexpected here"};
                emit(OpCode::LoadVar, dst, 0, 0, static_cast<int16_t>(str_const(u16(n.str))));
                break;
            }
            case NodeKind::ArrayLiteral: compile_array(n, dst); break;
            case NodeKind::ObjectLiteral: compile_object(n, dst); break;
            case NodeKind::Member: compile_member_get(n, dst); break;
            case NodeKind::Call: compile_call(n, dst); break;
            case NodeKind::New: compile_new(n, dst); break;
            case NodeKind::FunctionDeclaration:
            case NodeKind::ArrowFunction: compile_closure_expr(n, dst); break;
            case NodeKind::ClassDeclaration: compile_class(n, dst); break;
            case NodeKind::Unary: compile_unary(n, dst); break;
            case NodeKind::Binary: compile_binary(n, dst); break;
            case NodeKind::Logical: compile_logical(n, dst); break;
            case NodeKind::Assignment: compile_assignment(n, dst); break;
            case NodeKind::Conditional: compile_conditional(n, dst); break;
            case NodeKind::Await: {
                if (n.children.empty()) { emit(OpCode::LoadUndefined, dst, 0, 0, 0); break; }
                if (cur_->is_async) {
                    int mark = cur_->reg_top;
                    uint8_t v = alloc();
                    compile_expr(*n.children[0], v);
                    emit(OpCode::Await, dst, v, 0, 0);
                    free_to(mark);
                } else {
                    compile_expr(*n.children[0], dst);  // await outside async → value
                }
                break;
            }
            case NodeKind::Yield: compile_yield(n, dst); break;
            case NodeKind::Spread:
                if (!n.children.empty()) compile_expr(*n.children[0], dst);
                break;
            default:
                emit(OpCode::LoadUndefined, dst, 0, 0, 0);
                break;
        }
    }

    void compile_array(const Node& n, uint8_t dst) {
        bool has_spread = false;
        for (auto& c : n.children) if (c->kind == NodeKind::Spread) { has_spread = true; break; }
        if (has_spread) {
            emit(OpCode::NewArray, dst, 0, 0, 0);  // empty; append element-by-element
            for (auto& c : n.children) {
                int m = cur_->reg_top;
                uint8_t r = alloc();
                bool spread = c->kind == NodeKind::Spread;
                compile_expr(spread ? *c->children[0] : *c, r);
                emit(OpCode::ArrayAppend, dst, r, 0, spread ? 1 : 0);
                free_to(m);
            }
            return;
        }
        int count = static_cast<int>(n.children.size());
        int mark = cur_->reg_top;
        int base = alloc_block(count);
        for (int i = 0; i < count; ++i) compile_expr(*n.children[i], static_cast<uint8_t>(base + i));
        emit(OpCode::NewArray, dst, static_cast<uint16_t>(base), 0, static_cast<int16_t>(count));
        free_to(mark);
    }

    void compile_object(const Node& n, uint8_t dst) {
        emit(OpCode::NewObject, dst, 0, 0, 0);
        for (auto& prop : n.children) {
            int mark = cur_->reg_top;
            if (prop->kind == NodeKind::Spread) {           // { ...src }
                uint8_t rv = alloc();
                compile_expr(*prop->children[0], rv);
                emit(OpCode::CopyProps, dst, rv, 0, 0);
                free_to(mark);
                continue;
            }
            if (prop->flags & (parser::node_flags::ClassGetter | parser::node_flags::ClassSetter)) {
                uint8_t fnreg = alloc();
                int mi = add_function(compile_function_node(*prop->children[0], false, u16(prop->str)));
                emit(OpCode::NewClosure, fnreg, 0, 0, static_cast<int16_t>(mi));
                bool is_setter = prop->flags & parser::node_flags::ClassSetter;
                emit(OpCode::DefineAccessor, dst, fnreg, is_setter ? 1 : 0,
                     static_cast<int16_t>(str_const(u16(prop->str))));
                free_to(mark);
                continue;
            }
            uint8_t rv = alloc();
            if (!prop->children.empty()) compile_expr(*prop->children[0], rv);
            else emit(OpCode::LoadUndefined, rv, 0, 0, 0);
            if (prop->flags & parser::node_flags::Computed && prop->children.size() > 1) {
                uint8_t rk = alloc();
                compile_expr(*prop->children.back(), rk);
                emit(OpCode::SetElem, dst, rk, rv, 0);
            } else {
                emit(OpCode::SetProp, dst, rv, 0, static_cast<int16_t>(str_const(u16(prop->str))));
            }
            free_to(mark);
        }
    }

    // `super` only ever appears as the object of a Member or callee of a Call.
    static bool is_super(const Node& n) {
        return n.kind == NodeKind::Identifier && n.str == "super";
    }

    void compile_member_get(const Node& n, uint8_t dst) {
        int mark = cur_->reg_top;
        // super.prop / super[expr]: read off the superclass prototype.
        if (is_super(*n.children[0])) {
            uint8_t robj = alloc();
            emit(OpCode::LoadVar, robj, 0, 0, static_cast<int16_t>(str_const(u"%superproto%")));
            if (n.str == "[]") {
                uint8_t rkey = alloc();
                compile_expr(*n.children[1], rkey);
                emit(OpCode::GetElem, dst, robj, rkey, 0);
            } else {
                emit(OpCode::GetProp, dst, robj, 0, static_cast<int16_t>(str_const(u16(n.str))));
            }
            free_to(mark);
            return;
        }
        uint8_t robj = alloc();
        compile_expr(*n.children[0], robj);
        int jnull = -1;
        if (n.flags & parser::node_flags::Optional) {
            // a?.b : if a is null/undefined, the result is undefined (skip access).
            uint8_t nul = alloc(); emit(OpCode::LoadNull, nul, 0, 0, 0);
            uint8_t isn = alloc(); emit(OpCode::Eq, isn, robj, nul, 0);  // loose: null | undefined
            jnull = emit(OpCode::JumpIfTrue, 0, isn, 0, 0);
            free_to(static_cast<int>(robj) + 1);
        }
        if (n.str == "[]") {
            uint8_t rkey = alloc();
            compile_expr(*n.children[1], rkey);
            emit(OpCode::GetElem, dst, robj, rkey, 0);
        } else {
            emit(OpCode::GetProp, dst, robj, 0, static_cast<int16_t>(str_const(u16(n.str))));
        }
        if (jnull >= 0) {
            int jend = emit(OpCode::Jump, 0, 0, 0, 0);
            patch_imm(jnull, here());
            emit(OpCode::LoadUndefined, dst, 0, 0, 0);
            patch_imm(jend, here());
        }
        free_to(mark);
    }

    void compile_call(const Node& n, uint8_t dst) {
        const Node& callee = *n.children[0];

        // Optional call: f?.(args) / a.b?.(args) — skip the call if the callee is
        // null/undefined (the receiver still supplies `this`).
        if (n.flags & parser::node_flags::Optional) {
            int mark = cur_->reg_top;
            int argc = static_cast<int>(n.children.size()) - 1;
            uint8_t calleeReg = alloc();
            uint8_t thisReg = alloc();
            if (callee.kind == NodeKind::Member && !is_super(*callee.children[0])) {
                compile_expr(*callee.children[0], thisReg);
                if (callee.str == "[]") {
                    int m2 = cur_->reg_top; uint8_t rk = alloc();
                    compile_expr(*callee.children[1], rk);
                    emit(OpCode::GetElem, calleeReg, thisReg, rk, 0);
                    free_to(m2);
                } else {
                    emit(OpCode::GetProp, calleeReg, thisReg, 0, static_cast<int16_t>(str_const(u16(callee.str))));
                }
            } else {
                compile_expr(callee, calleeReg);
                emit(OpCode::LoadUndefined, thisReg, 0, 0, 0);
            }
            uint8_t nul = alloc(); emit(OpCode::LoadNull, nul, 0, 0, 0);
            uint8_t isn = alloc(); emit(OpCode::Eq, isn, calleeReg, nul, 0);
            int jnull = emit(OpCode::JumpIfTrue, 0, isn, 0, 0);
            free_to(static_cast<int>(thisReg) + 1);
            int base = alloc_block(2 + argc);
            emit(OpCode::Move, static_cast<uint8_t>(base), calleeReg, 0, 0);
            emit(OpCode::Move, static_cast<uint8_t>(base + 1), thisReg, 0, 0);
            for (int i = 0; i < argc; ++i)
                compile_expr(*n.children[i + 1], static_cast<uint8_t>(base + 2 + i));
            emit(OpCode::Call, dst, static_cast<uint16_t>(base), 0, static_cast<int16_t>(argc));
            int jend = emit(OpCode::Jump, 0, 0, 0, 0);
            patch_imm(jnull, here());
            emit(OpCode::LoadUndefined, dst, 0, 0, 0);
            patch_imm(jend, here());
            free_to(mark);
            return;
        }
        int argc = static_cast<int>(n.children.size()) - 1;
        int mark = cur_->reg_top;

        // super(args): invoke the superclass constructor on the current `this`.
        if (is_super(callee)) {
            int base = alloc_block(2 + argc);
            emit(OpCode::LoadVar, static_cast<uint8_t>(base), 0, 0,
                 static_cast<int16_t>(str_const(u"%super%")));
            emit(OpCode::LoadThis, static_cast<uint8_t>(base + 1), 0, 0, 0);
            for (int i = 0; i < argc; ++i)
                compile_expr(*n.children[i + 1], static_cast<uint8_t>(base + 2 + i));
            emit(OpCode::Call, dst, static_cast<uint16_t>(base), 0, static_cast<int16_t>(argc));
            free_to(mark);
            return;
        }
        // super.method(args): method off the superclass prototype, this = current this.
        if (callee.kind == NodeKind::Member && is_super(*callee.children[0])) {
            int base = alloc_block(2 + argc);
            emit(OpCode::LoadThis, static_cast<uint8_t>(base + 1), 0, 0, 0);
            int m2 = cur_->reg_top;
            uint8_t sproto = alloc();
            emit(OpCode::LoadVar, sproto, 0, 0, static_cast<int16_t>(str_const(u"%superproto%")));
            if (callee.str == "[]") {
                uint8_t rkey = alloc();
                compile_expr(*callee.children[1], rkey);
                emit(OpCode::GetElem, static_cast<uint8_t>(base), sproto, rkey, 0);
            } else {
                emit(OpCode::GetProp, static_cast<uint8_t>(base), sproto, 0,
                     static_cast<int16_t>(str_const(u16(callee.str))));
            }
            free_to(m2);
            for (int i = 0; i < argc; ++i)
                compile_expr(*n.children[i + 1], static_cast<uint8_t>(base + 2 + i));
            emit(OpCode::Call, dst, static_cast<uint16_t>(base), 0, static_cast<int16_t>(argc));
            free_to(mark);
            return;
        }

        // Argument spread: f(a, ...xs, b) — collect into an array and CallV.
        bool has_spread = false;
        for (int i = 1; i <= argc; ++i)
            if (n.children[i]->kind == NodeKind::Spread) { has_spread = true; break; }
        if (has_spread) {
            uint8_t calleeReg = alloc();
            uint8_t thisReg = alloc();
            if (callee.kind == NodeKind::Member) {
                compile_expr(*callee.children[0], thisReg);
                if (callee.str == "[]") {
                    int m2 = cur_->reg_top; uint8_t rkey = alloc();
                    compile_expr(*callee.children[1], rkey);
                    emit(OpCode::GetElem, calleeReg, thisReg, rkey, 0);
                    free_to(m2);
                } else {
                    emit(OpCode::GetProp, calleeReg, thisReg, 0,
                         static_cast<int16_t>(str_const(u16(callee.str))));
                }
            } else {
                compile_expr(callee, calleeReg);
                emit(OpCode::LoadUndefined, thisReg, 0, 0, 0);
            }
            uint8_t argsArr = alloc();
            build_args_array(argsArr, n, 1);
            emit(OpCode::CallV, dst, calleeReg, thisReg, static_cast<int16_t>(argsArr));
            free_to(mark);
            return;
        }

        int base = alloc_block(2 + argc);  // [callee][this][args...]

        if (callee.kind == NodeKind::Member) {
            uint8_t robj = static_cast<uint8_t>(base + 1);  // 'this' = receiver
            compile_expr(*callee.children[0], robj);
            if (callee.str == "[]") {
                int m2 = cur_->reg_top; uint8_t rkey = alloc();
                compile_expr(*callee.children[1], rkey);
                emit(OpCode::GetElem, static_cast<uint8_t>(base), robj, rkey, 0);
                free_to(m2);
            } else {
                emit(OpCode::GetProp, static_cast<uint8_t>(base), robj, 0,
                     static_cast<int16_t>(str_const(u16(callee.str))));
            }
        } else {
            compile_expr(callee, static_cast<uint8_t>(base));
            emit(OpCode::LoadUndefined, static_cast<uint8_t>(base + 1), 0, 0, 0);
        }
        for (int i = 0; i < argc; ++i)
            compile_expr(*n.children[i + 1], static_cast<uint8_t>(base + 2 + i));

        emit(OpCode::Call, dst, static_cast<uint16_t>(base), 0, static_cast<int16_t>(argc));
        free_to(mark);
    }

    void compile_new(const Node& n, uint8_t dst) {
        const Node* callee = n.children.empty() ? nullptr : n.children[0].get();
        std::vector<const Node*> args;
        if (callee && callee->kind == NodeKind::Call) {
            for (size_t i = 1; i < callee->children.size(); ++i) args.push_back(callee->children[i].get());
            callee = callee->children[0].get();
        }
        int argc = static_cast<int>(args.size());
        int mark = cur_->reg_top;

        bool has_spread = false;
        for (const Node* a : args) if (a->kind == NodeKind::Spread) { has_spread = true; break; }
        if (has_spread) {
            uint8_t calleeReg = alloc();
            if (callee) compile_expr(*callee, calleeReg);
            else emit(OpCode::LoadUndefined, calleeReg, 0, 0, 0);
            uint8_t argsArr = alloc();
            emit(OpCode::NewArray, argsArr, 0, 0, 0);
            for (const Node* a : args) {
                int m = cur_->reg_top; uint8_t r = alloc();
                bool spread = a->kind == NodeKind::Spread;
                compile_expr(spread ? *a->children[0] : *a, r);
                emit(OpCode::ArrayAppend, argsArr, r, 0, spread ? 1 : 0);
                free_to(m);
            }
            emit(OpCode::ConstructV, dst, calleeReg, 0, static_cast<int16_t>(argsArr));
            free_to(mark);
            return;
        }

        int base = alloc_block(1 + argc);  // [callee][args...]
        if (callee) compile_expr(*callee, static_cast<uint8_t>(base));
        else emit(OpCode::LoadUndefined, static_cast<uint8_t>(base), 0, 0, 0);
        for (int i = 0; i < argc; ++i)
            compile_expr(*args[i], static_cast<uint8_t>(base + 1 + i));
        emit(OpCode::Construct, dst, static_cast<uint16_t>(base), 0, static_cast<int16_t>(argc));
        free_to(mark);
    }

    // Build a fresh array in `dst` from call/array args starting at index `start`,
    // expanding any Spread element. Used for argument spread (CallV/ConstructV).
    void build_args_array(uint8_t dst, const Node& n, int start) {
        emit(OpCode::NewArray, dst, 0, 0, 0);
        for (int i = start; i < static_cast<int>(n.children.size()); ++i) {
            const Node& a = *n.children[i];
            int m = cur_->reg_top; uint8_t r = alloc();
            bool spread = a.kind == NodeKind::Spread;
            compile_expr(spread ? *a.children[0] : a, r);
            emit(OpCode::ArrayAppend, dst, r, 0, spread ? 1 : 0);
            free_to(m);
        }
    }

    void compile_yield(const Node& n, uint8_t dst) {
        if (n.str == "*") {
            // yield* iterable: materialize via ToIterable, then yield each element.
            int mark = cur_->reg_top;
            uint8_t src = alloc();
            if (!n.children.empty()) compile_expr(*n.children[0], src);
            else emit(OpCode::LoadUndefined, src, 0, 0, 0);
            uint8_t arr = alloc();
            emit(OpCode::ToIterable, arr, src, 0, 0);
            uint8_t idx = alloc();
            emit(OpCode::LoadConst, idx, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(0))));
            uint8_t len = alloc();
            emit(OpCode::GetLength, len, arr, 0, 0);
            int loop = here();
            uint8_t cond = alloc();
            emit(OpCode::Lt, cond, idx, len, 0);
            int jend = emit(OpCode::JumpIfFalse, 0, cond, 0, 0);
            uint8_t elem = alloc();
            emit(OpCode::GetElem, elem, arr, idx, 0);
            uint8_t ig = alloc();
            emit(OpCode::Yield, ig, elem, 0, 0);
            uint8_t one = alloc();
            emit(OpCode::LoadConst, one, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(1))));
            emit(OpCode::Add, idx, idx, one, 0);
            emit(OpCode::Jump, 0, 0, 0, static_cast<int16_t>(loop));
            patch_imm(jend, here());
            emit(OpCode::LoadUndefined, dst, 0, 0, 0);
            free_to(mark);
            return;
        }
        int mark = cur_->reg_top;
        uint8_t v = alloc();
        if (!n.children.empty()) compile_expr(*n.children[0], v);
        else emit(OpCode::LoadUndefined, v, 0, 0, 0);
        emit(OpCode::Yield, dst, v, 0, 0);
        free_to(mark);
    }

    void compile_closure_expr(const Node& n, uint8_t dst) {
        auto nested = compile_function_node(n, false, u16(n.str));
        int fidx = static_cast<int>(cur_->fn->functions.size());
        cur_->fn->functions.push_back(nested);
        emit(OpCode::NewClosure, dst, 0, 0, static_cast<int16_t>(fidx));
    }

    int add_function(std::shared_ptr<Function> fn) {
        int idx = static_cast<int>(cur_->fn->functions.size());
        cur_->fn->functions.push_back(std::move(fn));
        return idx;
    }

    // Lower an ES6 class into a constructor function plus prototype/static method
    // installation and (for `extends`) prototype-chain + static-base wiring. The
    // constructor and every method are closures created in a scope that binds
    // %super% (the superclass constructor) and %superproto% (its prototype), so
    // `super(...)` / `super.method(...)` resolve lexically. Instance fields are
    // injected into the constructor (after super() for derived classes).
    void compile_class(const Node& n, uint8_t dst) {
        const Node& heritage = *n.children[0];
        bool has_super = heritage.kind != NodeKind::UndefinedLiteral;

        const Node* ctor_member = nullptr;
        std::vector<const Node*> methods, statics, fields, static_fields, accessors, static_accessors;
        for (size_t i = 1; i < n.children.size(); ++i) {
            const Node* m = n.children[i].get();
            bool is_field  = m->flags & parser::node_flags::ClassField;
            bool is_static = m->flags & parser::node_flags::ClassStatic;
            bool is_accessor = m->flags & (parser::node_flags::ClassGetter | parser::node_flags::ClassSetter);
            if (is_accessor)            { (is_static ? static_accessors : accessors).push_back(m); continue; }
            if (!is_field && !is_static && m->str == "constructor") { ctor_member = m; continue; }
            if (is_field)  (is_static ? static_fields : fields).push_back(m);
            else           (is_static ? statics : methods).push_back(m);
        }

        int mark = cur_->reg_top;

        uint8_t superReg = 0;
        if (has_super) { superReg = alloc(); compile_expr(heritage, superReg); }

        // Scope holding %super% / %superproto% for closures to capture.
        if (has_super) {
            emit(OpCode::PushScope, 0, 0, 0, 0);
            emit(OpCode::DefineVar, 0, superReg, 0, static_cast<int16_t>(str_const(u"%super%")));
            uint8_t sproto = alloc();
            emit(OpCode::GetProp, sproto, superReg, 0, static_cast<int16_t>(str_const(u"prototype")));
            emit(OpCode::DefineVar, 0, sproto, 0, static_cast<int16_t>(str_const(u"%superproto%")));
        }

        // Constructor closure (its .prototype is auto-created by NewClosure).
        const Node* ctorFnNode = ctor_member ? ctor_member->children[0].get() : nullptr;
        int cfidx = add_function(compile_constructor(ctorFnNode, n.str, has_super, fields));
        uint8_t ctorReg = alloc();
        emit(OpCode::NewClosure, ctorReg, 0, 0, static_cast<int16_t>(cfidx));
        uint8_t protoReg = alloc();
        emit(OpCode::GetProp, protoReg, ctorReg, 0, static_cast<int16_t>(str_const(u"prototype")));

        int body_mark = cur_->reg_top;
        auto is_computed = [](const Node* m) { return (m->flags & parser::node_flags::Computed) != 0; };
        auto install = [&](const Node* m, uint8_t target) {
            const Node& fnNode = *m->children[0];
            int mi = add_function(compile_function_node(fnNode, false, is_computed(m) ? u"" : u16(m->str)));
            uint8_t mr = alloc();
            emit(OpCode::NewClosure, mr, 0, 0, static_cast<int16_t>(mi));
            // Class methods are non-enumerable own properties (SetProp src_b=1 /
            // SetElem imm16=1 select non-enumerable).
            if (is_computed(m)) {
                uint8_t kr = alloc();
                compile_expr(*m->children.back(), kr);
                emit(OpCode::SetElem, target, kr, mr, 1);
            } else {
                emit(OpCode::SetProp, target, mr, 1, static_cast<int16_t>(str_const(u16(m->str))));
            }
            free_to(body_mark);
        };
        auto install_accessor = [&](const Node* m, uint8_t target) {
            const Node& fnNode = *m->children[0];
            int mi = add_function(compile_function_node(fnNode, false, is_computed(m) ? u"" : u16(m->str)));
            uint8_t mr = alloc();
            emit(OpCode::NewClosure, mr, 0, 0, static_cast<int16_t>(mi));
            bool is_setter = m->flags & parser::node_flags::ClassSetter;
            if (is_computed(m)) {
                uint8_t kr = alloc();
                compile_expr(*m->children.back(), kr);
                emit(OpCode::DefineAccessorV, target, mr, kr, is_setter ? 1 : 0);
            } else {
                emit(OpCode::DefineAccessor, target, mr, is_setter ? 1 : 0,
                     static_cast<int16_t>(str_const(u16(m->str))));
            }
            free_to(body_mark);
        };
        for (const Node* m : methods) install(m, protoReg);
        for (const Node* m : statics) install(m, ctorReg);
        for (const Node* m : accessors) install_accessor(m, protoReg);
        for (const Node* m : static_accessors) install_accessor(m, ctorReg);
        for (const Node* m : static_fields) {
            uint8_t fv = alloc();
            compile_expr(*m->children[0], fv);
            if (is_computed(m)) {
                uint8_t kr = alloc();
                compile_expr(*m->children.back(), kr);
                emit(OpCode::SetElem, ctorReg, kr, fv, 0);
            } else {
                emit(OpCode::SetProp, ctorReg, fv, 0, static_cast<int16_t>(str_const(u16(m->str))));
            }
            free_to(body_mark);
        }

        if (has_super) {
            // Instance method inheritance + static-base chain.
            emit(OpCode::SetProto, protoReg, superReg, 0, 0);
            emit(OpCode::SetProto, ctorReg, superReg, 0, 0);
        }

        emit(OpCode::Move, dst, ctorReg, 0, 0);
        if (has_super) emit(OpCode::PopScope, 0, 0, 0, 0);
        free_to(mark);
    }

    static bool is_super_call_stmt(const Node& s) {
        if (s.kind != NodeKind::ExpressionStatement || s.children.empty()) return false;
        const Node& e = *s.children[0];
        return e.kind == NodeKind::Call && !e.children.empty() && is_super(*e.children[0]);
    }

    // Builds the constructor Function. `ctorFnNode` is the explicit constructor's
    // FunctionDeclaration (params... + body) or null for a synthesized default.
    std::shared_ptr<Function> compile_constructor(const Node* ctorFnNode,
                                                  const std::string& class_name,
                                                  bool has_super,
                                                  const std::vector<const Node*>& fields) {
        auto fn = std::make_shared<Function>();
        // Anonymous classes start nameless so that named evaluation can fill it in
        // (`const C = class {}` => C.name === "C"); the empty name reads as "".
        fn->name = class_name.empty() ? u"" : u16(class_name);

        FnCtx ctx; ctx.fn = fn.get();
        FnCtx* saved = cur_;
        cur_ = &ctx;

        const Node* body = nullptr;
        if (ctorFnNode) {
            size_t nparams = ctorFnNode->children.empty() ? 0 : ctorFnNode->children.size() - 1;
            for (size_t i = 0; i < nparams; ++i)
                fn->param_names.push_back(u16(ctorFnNode->children[i]->str));
            fn->arity = static_cast<uint32_t>(nparams);
            body = ctorFnNode->children.empty() ? nullptr : ctorFnNode->children.back().get();
        }

        auto emit_fields = [&]() {
            for (const Node* f : fields) {
                int fm = cur_->reg_top;
                uint8_t rv = alloc();
                compile_expr(*f->children[0], rv);
                uint8_t rthis = alloc();
                emit(OpCode::LoadThis, rthis, 0, 0, 0);
                if (f->flags & parser::node_flags::Computed) {
                    uint8_t kr = alloc();
                    compile_expr(*f->children.back(), kr);
                    emit(OpCode::SetElem, rthis, kr, rv, 0);
                } else {
                    emit(OpCode::SetProp, rthis, rv, 0, static_cast<int16_t>(str_const(u16(f->str))));
                }
                free_to(fm);
            }
        };

        if (!has_super) {
            emit_fields();
            if (body && body->kind == NodeKind::Block) {
                hoist(*body);
                for (auto& s : body->children) compile_stmt(*s);
            }
        } else if (!ctorFnNode) {
            // Default derived constructor: super(...arguments); <fields>
            int fm = cur_->reg_top;
            uint8_t callee = alloc();
            emit(OpCode::LoadVar, callee, 0, 0, static_cast<int16_t>(str_const(u"%super%")));
            uint8_t rthis = alloc();
            emit(OpCode::LoadThis, rthis, 0, 0, 0);
            uint8_t rargs = alloc();
            emit(OpCode::LoadVar, rargs, 0, 0, static_cast<int16_t>(str_const(u"arguments")));
            uint8_t res = alloc();
            emit(OpCode::CallV, res, callee, rthis, static_cast<int16_t>(rargs));
            free_to(fm);
            emit_fields();
        } else if (body && body->kind == NodeKind::Block) {
            // Explicit derived constructor: inject fields right after super().
            bool has_super_stmt = false;
            for (auto& s : body->children) if (is_super_call_stmt(*s)) { has_super_stmt = true; break; }
            hoist(*body);
            if (!has_super_stmt) emit_fields();
            bool emitted = false;
            for (auto& s : body->children) {
                compile_stmt(*s);
                if (!emitted && is_super_call_stmt(*s)) { emit_fields(); emitted = true; }
            }
        }
        emit(OpCode::ReturnUndefined, 0, 0, 0, 0);

        cur_ = saved;
        return fn;
    }

    void compile_unary(const Node& n, uint8_t dst) {
        const std::string& op = n.str;
        if (op == "++" || op == "--" || op == "post++" || op == "post--") {
            compile_inc_dec(n, dst);
            return;
        }
        if (op == "delete") {
            const Node& tgt = *n.children[0];
            if (tgt.kind == NodeKind::Member) {
                int m = cur_->reg_top;
                uint8_t obj = alloc();
                compile_expr(*tgt.children[0], obj);
                if (tgt.str == "[]") {
                    uint8_t key = alloc();
                    compile_expr(*tgt.children[1], key);
                    emit(OpCode::DeleteElem, dst, obj, key, 0);
                } else {
                    emit(OpCode::DeleteProp, dst, obj, 0, static_cast<int16_t>(str_const(u16(tgt.str))));
                }
                free_to(m);
            } else {
                emit(OpCode::LoadBool, dst, 0, 0, 1);  // delete of a non-reference => true
            }
            return;
        }
        // `typeof <identifier>` must yield "undefined" for an unresolved name
        // rather than throwing ReferenceError (the basis of feature detection).
        if (op == "typeof" && n.children[0]->kind == NodeKind::Identifier) {
            int m = cur_->reg_top;
            uint8_t r = alloc();
            emit(OpCode::LoadVarOrUndef, r, 0, 0, static_cast<int16_t>(str_const(u16(n.children[0]->str))));
            emit(OpCode::TypeOf, dst, r, 0, 0);
            free_to(m);
            return;
        }
        int mark = cur_->reg_top;
        uint8_t r = alloc();
        compile_expr(*n.children[0], r);
        if (op == "!") emit(OpCode::LogNot, dst, r, 0, 0);
        else if (op == "-") emit(OpCode::Neg, dst, r, 0, 0);
        else if (op == "~") emit(OpCode::BitNot, dst, r, 0, 0);
        else if (op == "typeof") emit(OpCode::TypeOf, dst, r, 0, 0);
        else if (op == "void") emit(OpCode::LoadUndefined, dst, 0, 0, 0);
        else if (op == "+") { uint8_t one = alloc(); emit(OpCode::LoadConst, one, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(1)))); emit(OpCode::Mul, dst, r, one, 0); }
        else emit(OpCode::Move, dst, r, 0, 0);
        free_to(mark);
    }

    void compile_inc_dec(const Node& n, uint8_t dst) {
        bool postfix = n.str.rfind("post", 0) == 0;
        bool inc = n.str.find("++") != std::string::npos;
        const Node& target = *n.children[0];
        int mark = cur_->reg_top;
        uint8_t old = alloc();
        compile_expr(target, old);
        uint8_t one = alloc();
        emit(OpCode::LoadConst, one, 0, 0, static_cast<int16_t>(num_const(vm::Value::make_int32(1))));
        uint8_t nv = alloc();
        emit(inc ? OpCode::Add : OpCode::Sub, nv, old, one, 0);
        store_to_target(target, nv);
        emit(OpCode::Move, dst, postfix ? old : nv, 0, 0);
        free_to(mark);
    }

    void compile_binary(const Node& n, uint8_t dst) {
        int mark = cur_->reg_top;
        uint8_t a = alloc();
        compile_expr(*n.children[0], a);
        uint8_t b = alloc();
        compile_expr(*n.children[1], b);
        const std::string& op = n.str;
        OpCode oc = OpCode::Add;
        if (op == "+") oc = OpCode::Add; else if (op == "-") oc = OpCode::Sub;
        else if (op == "*") oc = OpCode::Mul; else if (op == "/") oc = OpCode::Div;
        else if (op == "%") oc = OpCode::Mod; else if (op == "**") oc = OpCode::Pow;
        else if (op == "==") oc = OpCode::Eq; else if (op == "!=") oc = OpCode::NEq;
        else if (op == "===") oc = OpCode::StrictEq; else if (op == "!==") oc = OpCode::StrictNEq;
        else if (op == "<") oc = OpCode::Lt; else if (op == "<=") oc = OpCode::Lte;
        else if (op == ">") oc = OpCode::Gt; else if (op == ">=") oc = OpCode::Gte;
        else if (op == "&") oc = OpCode::BitAnd; else if (op == "|") oc = OpCode::BitOr;
        else if (op == "^") oc = OpCode::BitXor; else if (op == "<<") oc = OpCode::Shl;
        else if (op == ">>") oc = OpCode::Shr; else if (op == ">>>") oc = OpCode::UShr;
        else if (op == "instanceof") oc = OpCode::InstanceOf; else if (op == "in") oc = OpCode::In;
        else throw CompileError{"unsupported binary operator '" + op + "'"};
        emit(oc, dst, a, b, 0);
        free_to(mark);
    }

    void compile_logical(const Node& n, uint8_t dst) {
        const std::string& op = n.str;
        compile_expr(*n.children[0], dst);
        if (op == "&&") {
            int j = emit(OpCode::JumpIfFalse, 0, dst, 0, 0);
            compile_expr(*n.children[1], dst);
            patch_imm(j, here());
        } else if (op == "||") {
            int j = emit(OpCode::JumpIfTrue, 0, dst, 0, 0);
            compile_expr(*n.children[1], dst);
            patch_imm(j, here());
        } else {  // ?? nullish
            int mark = cur_->reg_top;
            uint8_t tmp = alloc();
            emit(OpCode::LoadNull, tmp, 0, 0, 0);
            uint8_t isnull = alloc();
            emit(OpCode::StrictEq, isnull, dst, tmp, 0);
            int jr1 = emit(OpCode::JumpIfTrue, 0, isnull, 0, 0);
            emit(OpCode::LoadUndefined, tmp, 0, 0, 0);
            emit(OpCode::StrictEq, isnull, dst, tmp, 0);
            int jr2 = emit(OpCode::JumpIfTrue, 0, isnull, 0, 0);
            int jend = emit(OpCode::Jump, 0, 0, 0, 0);
            patch_imm(jr1, here()); patch_imm(jr2, here());
            compile_expr(*n.children[1], dst);
            patch_imm(jend, here());
            free_to(mark);
        }
    }

    void compile_conditional(const Node& n, uint8_t dst) {
        int mark = cur_->reg_top;
        uint8_t c = alloc();
        compile_expr(*n.children[0], c);
        int jfalse = emit(OpCode::JumpIfFalse, 0, c, 0, 0);
        free_to(mark);
        compile_expr(*n.children[1], dst);
        int jend = emit(OpCode::Jump, 0, 0, 0, 0);
        patch_imm(jfalse, here());
        compile_expr(*n.children[2], dst);
        patch_imm(jend, here());
    }

    void compile_assignment(const Node& n, uint8_t dst) {
        const Node& target = *n.children[0];
        const std::string& op = n.str;
        if (op == "=") {
            // Destructuring assignment: `[a, b] = v` / `({x} = v)`.
            if (target.kind == NodeKind::ArrayLiteral || target.kind == NodeKind::ObjectLiteral) {
                compile_expr(*n.children[1], dst);
                bind_pattern_or_target(target, dst, /*declare*/false, /*is_var*/false);
                return;
            }
            if (target.kind == NodeKind::Identifier)
                compile_init_named(*n.children[1], dst, u16(target.str));  // named evaluation
            else
                compile_expr(*n.children[1], dst);
            store_to_target(target, dst);
            return;
        }
        // Logical assignment (short-circuit): a &&= b / a ||= b / a ??= b only
        // evaluate and store b when the gate passes.
        if (op == "&&=" || op == "||=" || op == "?\?=") {
            compile_expr(target, dst);
            int jskip;
            if (op == "&&=") {
                jskip = emit(OpCode::JumpIfFalse, 0, dst, 0, 0);
            } else if (op == "||=") {
                jskip = emit(OpCode::JumpIfTrue, 0, dst, 0, 0);
            } else {  // ??= : assign only when dst is null or undefined (loose == null)
                int mark = cur_->reg_top;
                uint8_t nul = alloc(); emit(OpCode::LoadNull, nul, 0, 0, 0);
                uint8_t isn = alloc(); emit(OpCode::Eq, isn, dst, nul, 0);  // loose: true for null & undefined
                jskip = emit(OpCode::JumpIfFalse, 0, isn, 0, 0);
                free_to(mark);
            }
            compile_expr(*n.children[1], dst);
            store_to_target(target, dst);
            patch_imm(jskip, here());
            return;
        }
        // compound: target op= value  =>  target = target <op> value
        int mark = cur_->reg_top;
        uint8_t cur_v = alloc();
        compile_expr(target, cur_v);
        uint8_t rhs = alloc();
        compile_expr(*n.children[1], rhs);
        OpCode oc = OpCode::Add;
        std::string bop = op.substr(0, op.size() - 1);  // strip '='
        if (bop == "+") oc = OpCode::Add; else if (bop == "-") oc = OpCode::Sub;
        else if (bop == "*") oc = OpCode::Mul; else if (bop == "/") oc = OpCode::Div;
        else if (bop == "%") oc = OpCode::Mod; else if (bop == "**") oc = OpCode::Pow;
        else if (bop == "&") oc = OpCode::BitAnd; else if (bop == "|") oc = OpCode::BitOr;
        else if (bop == "^") oc = OpCode::BitXor; else if (bop == "<<") oc = OpCode::Shl;
        else if (bop == ">>") oc = OpCode::Shr; else if (bop == ">>>") oc = OpCode::UShr;
        else throw CompileError{"unsupported assignment operator '" + op + "'"};
        emit(oc, dst, cur_v, rhs, 0);
        store_to_target(target, dst);
        free_to(mark);
    }

    void store_to_target(const Node& target, uint8_t value_reg) {
        if (target.kind == NodeKind::Identifier) {
            emit(OpCode::StoreVar, 0, value_reg, 0, static_cast<int16_t>(str_const(u16(target.str))));
        } else if (target.kind == NodeKind::Member) {
            int mark = cur_->reg_top;
            uint8_t robj = alloc();
            compile_expr(*target.children[0], robj);
            if (target.str == "[]") {
                uint8_t rkey = alloc();
                compile_expr(*target.children[1], rkey);
                emit(OpCode::SetElem, robj, rkey, value_reg, 0);
            } else {
                emit(OpCode::SetProp, robj, value_reg, 0, static_cast<int16_t>(str_const(u16(target.str))));
            }
            free_to(mark);
        } else {
            throw CompileError{"invalid assignment target"};
        }
    }

    std::set<const Node*> hoisted_fns_;
};

} // namespace

Compiler::Result Compiler::compile(const parser::Node& program) {
    Impl impl;
    return impl.run(program);
}

} // namespace malibu::js::compiler
