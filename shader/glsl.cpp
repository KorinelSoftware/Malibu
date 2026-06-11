// shader/glsl.cpp — MalibuShader: GLSL ES lexer + parser + tree-walk interpreter.
#include "malibu/shader/glsl.h"

#include <cmath>
#include <cstdlib>
#include <functional>

namespace malibu::shader {
namespace {

// ---- lexer ----
enum class Tk { End, Id, Num, Punct };
struct Token { Tk k; std::string s; double num = 0; };

struct Lexer {
    const std::string& src; size_t i = 0;
    explicit Lexer(const std::string& s) : src(s) {}
    void skip() {
        while (i < src.size()) {
            char c = src[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
            if (c == '/' && i + 1 < src.size() && src[i+1] == '/') { while (i < src.size() && src[i] != '\n') i++; continue; }
            if (c == '/' && i + 1 < src.size() && src[i+1] == '*') { i += 2; while (i + 1 < src.size() && !(src[i]=='*'&&src[i+1]=='/')) i++; i += 2; continue; }
            break;
        }
    }
    Token next() {
        skip();
        if (i >= src.size()) return {Tk::End, ""};
        char c = src[i];
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t s = i; while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i]=='_')) i++;
            return {Tk::Id, src.substr(s, i - s)};
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i+1 < src.size() && std::isdigit((unsigned char)src[i+1]))) {
            size_t s = i; while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i]=='.')) i++;
            return {Tk::Num, src.substr(s, i - s), std::atof(src.substr(s, i - s).c_str())};
        }
        // multi-char punctuators
        static const char* multi[] = {"==","!=","<=",">=","&&","||","+=","-=","*=","/="};
        for (auto* m : multi) if (src.compare(i, 2, m) == 0) { i += 2; return {Tk::Punct, m}; }
        i++; return {Tk::Punct, std::string(1, c)};
    }
};

}  // namespace

// ---- AST ----
struct Node {
    enum K { Num, Id, Bin, Un, Call, Member, Index, Assign, Decl, If, Ret, Block, Expr, Func } k;
    std::string s;         // operator / name / type
    double num = 0;
    std::vector<std::shared_ptr<Node>> kids;
};
using NP = std::shared_ptr<Node>;

namespace {
NP mk(Node::K k) { auto n = std::make_shared<Node>(); n->k = k; return n; }

// ---- parser ----
struct Parser {
    std::vector<Token> toks; size_t p = 0;
    std::string err;

    explicit Parser(const std::string& src) {
        Lexer lx(src); for (Token t = lx.next(); t.k != Tk::End; t = lx.next()) toks.push_back(t);
        toks.push_back({Tk::End, ""});
    }
    Token& cur() { return toks[p]; }
    bool is(const std::string& s) { return cur().s == s; }
    bool accept(const std::string& s) { if (is(s)) { p++; return true; } return false; }
    bool expect(const std::string& s) { if (accept(s)) return true; if (err.empty()) err = "expected '" + s + "' got '" + cur().s + "'"; return false; }
    bool is_type(const std::string& s) {
        return s=="void"||s=="float"||s=="int"||s=="bool"||s=="vec2"||s=="vec3"||s=="vec4"||
               s=="mat2"||s=="mat3"||s=="mat4"||s=="sampler2D";
    }

    NP parse_program(std::vector<std::string>& attrs, std::vector<std::string>& varys, std::vector<std::string>& unis) {
        auto root = mk(Node::Block);
        while (cur().k != Tk::End && err.empty()) {
            // qualifier?
            std::string q;
            if (is("attribute")||is("uniform")||is("varying")||is("const")) { q = cur().s; p++; }
            if (is("precision")) { while (!is(";") && cur().k != Tk::End) p++; accept(";"); continue; }
            if (cur().k == Tk::Id && is_type(cur().s)) {
                std::string type = cur().s; p++;
                std::string name = cur().s; p++;
                if (is("(")) {  // function
                    auto fn = parse_func(type, name);
                    if (fn) root->kids.push_back(fn);
                } else {  // global var decl
                    if (q == "attribute") attrs.push_back(name);
                    else if (q == "varying") varys.push_back(name);
                    else if (q == "uniform") unis.push_back(name);
                    while (!is(";") && cur().k != Tk::End) p++;  // skip initializer/array
                    accept(";");
                }
            } else { p++; }  // skip stray token
        }
        return root;
    }
    NP parse_func(const std::string& /*rtype*/, const std::string& name) {
        auto fn = mk(Node::Func); fn->s = name;
        expect("(");
        while (!is(")") && cur().k != Tk::End) p++;  // params ignored (we seed via inputs)
        expect(")");
        fn->kids.push_back(parse_block());
        return fn;
    }
    NP parse_block() {
        auto b = mk(Node::Block);
        if (!expect("{")) return b;
        while (!is("}") && cur().k != Tk::End && err.empty()) {
            auto s = parse_stmt();
            if (s) b->kids.push_back(s);
        }
        expect("}");
        return b;
    }
    NP parse_stmt() {
        if (is("{")) return parse_block();
        if (accept("if")) {
            auto n = mk(Node::If); expect("("); n->kids.push_back(parse_expr()); expect(")");
            n->kids.push_back(parse_stmt());
            if (accept("else")) n->kids.push_back(parse_stmt());
            return n;
        }
        if (accept("return")) { auto n = mk(Node::Ret); if (!is(";")) n->kids.push_back(parse_expr()); accept(";"); return n; }
        if (is("for")||is("while")) { while (!is("{")&&cur().k!=Tk::End) p++; return parse_block(); }  // loops: skipped subset
        if (cur().k == Tk::Id && is_type(cur().s)) {  // local decl
            std::string type = cur().s; p++;
            auto n = mk(Node::Decl); n->s = type;
            n->num = 0; n->kids.push_back(nullptr);  // placeholder name node
            auto nameNode = mk(Node::Id); nameNode->s = cur().s; p++;
            n->kids[0] = nameNode;
            if (accept("=")) n->kids.push_back(parse_expr());
            accept(";");
            return n;
        }
        // expression statement (often an assignment)
        auto e = parse_expr(); accept(";");
        auto st = mk(Node::Expr); st->kids.push_back(e); return st;
    }

    NP parse_expr() { return parse_assign(); }
    NP parse_assign() {
        auto lhs = parse_add();
        if (is("=")||is("+=")||is("-=")||is("*=")||is("/=")) {
            std::string op = cur().s; p++;
            auto rhs = parse_assign();
            auto n = mk(Node::Assign); n->s = op; n->kids = {lhs, rhs}; return n;
        }
        return lhs;
    }
    NP parse_add() {
        auto l = parse_mul();
        while (is("+")||is("-")) { std::string op = cur().s; p++; auto r = parse_mul(); auto n = mk(Node::Bin); n->s = op; n->kids = {l, r}; l = n; }
        return l;
    }
    NP parse_mul() {
        auto l = parse_unary();
        while (is("*")||is("/")) { std::string op = cur().s; p++; auto r = parse_unary(); auto n = mk(Node::Bin); n->s = op; n->kids = {l, r}; l = n; }
        return l;
    }
    NP parse_unary() {
        if (is("-")||is("+")||is("!")) { std::string op = cur().s; p++; auto n = mk(Node::Un); n->s = op; n->kids = {parse_unary()}; return n; }
        return parse_postfix();
    }
    NP parse_postfix() {
        auto e = parse_primary();
        while (true) {
            if (accept(".")) { auto n = mk(Node::Member); n->s = cur().s; p++; n->kids = {e}; e = n; }
            else if (accept("[")) { auto n = mk(Node::Index); n->kids = {e, parse_expr()}; expect("]"); e = n; }
            else break;
        }
        return e;
    }
    NP parse_primary() {
        if (cur().k == Tk::Num) { auto n = mk(Node::Num); n->num = cur().num; p++; return n; }
        if (accept("(")) { auto e = parse_expr(); expect(")"); return e; }
        if (cur().k == Tk::Id) {
            std::string name = cur().s; p++;
            if (is("(")) {  // call / constructor
                auto n = mk(Node::Call); n->s = name; expect("(");
                if (!is(")")) { do { n->kids.push_back(parse_expr()); } while (accept(",")); }
                expect(")");
                return n;
            }
            auto n = mk(Node::Id); n->s = name; return n;
        }
        p++; auto n = mk(Node::Num); return n;  // error recovery
    }
};

// ---- interpreter ----
int swizzle_index(char c) {
    switch (c) { case 'x': case 'r': case 's': return 0; case 'y': case 'g': case 't': return 1;
                 case 'z': case 'b': case 'p': return 2; case 'w': case 'a': case 'q': return 3; }
    return 0;
}
Val make_vec(std::vector<float> comps) { Val r; r.n = (int)std::min<size_t>(comps.size(), 4); for (int i = 0; i < r.n; i++) r.v[i] = comps[i]; return r; }

struct Interp {
    std::map<std::string, Val>& env;
    const Program::Sampler* sampler = nullptr;
    explicit Interp(std::map<std::string, Val>& e) : env(e) {}

    Val eval(const NP& n);
    void exec(const NP& n) {
        if (!n) return;
        switch (n->k) {
            case Node::Block: for (auto& k : n->kids) exec(k); break;
            case Node::Expr: eval(n->kids[0]); break;
            case Node::Decl: {
                Val v; if (n->kids.size() > 1) v = eval(n->kids[1]);
                env[n->kids[0]->s] = v; break;
            }
            case Node::Assign: {
                Val r = eval(n->kids[1]);
                const NP& lhs = n->kids[0];
                if (n->s != "=") {  // compound
                    Val cur = eval(lhs);
                    r = binop(n->s.substr(0,1), cur, r);
                }
                assign(lhs, r); break;
            }
            case Node::If: {
                Val c = eval(n->kids[0]);
                if (c.v[0] != 0) exec(n->kids[1]); else if (n->kids.size() > 2) exec(n->kids[2]); break;
            }
            case Node::Ret: break;  // (only main(): return ignored)
            default: eval(n); break;
        }
    }
    void assign(const NP& lhs, const Val& r) {
        if (lhs->k == Node::Id) { env[lhs->s] = merge(env.count(lhs->s)?env[lhs->s]:Val{}, r, -1); return; }
        if (lhs->k == Node::Member) {  // a.xy = ...  (write swizzle components)
            Val base = env.count(lhs->kids[0]->s) ? env[lhs->kids[0]->s] : Val{};
            for (size_t i = 0; i < lhs->s.size() && (int)i < r.n; i++) base.v[swizzle_index(lhs->s[i])] = r.v[i];
            if (r.n == 1) for (char c : lhs->s) base.v[swizzle_index(c)] = r.v[0];
            base.n = std::max(base.n, (int)lhs->s.size());
            env[lhs->kids[0]->s] = base; return;
        }
        if (lhs->k == Node::Id) env[lhs->s] = r;
    }
    Val merge(Val, const Val& r, int) { return r; }

    Val binop(const std::string& op, Val a, Val b);
    Val call(const std::string& name, std::vector<Val>& args);
};

Val Interp::eval(const NP& n) {
    if (!n) return Val{};
    switch (n->k) {
        case Node::Num: return Val::scalar((float)n->num);
        case Node::Id: {
            if (n->s == "true") return Val::scalar(1);
            if (n->s == "false") return Val::scalar(0);
            auto it = env.find(n->s); return it != env.end() ? it->second : Val{};
        }
        case Node::Un: { Val v = eval(n->kids[0]); if (n->s == "-") for (int i = 0; i < 16; i++) v.v[i] = -v.v[i]; if (n->s == "!") v.v[0] = v.v[0] == 0 ? 1 : 0; return v; }
        case Node::Bin: { Val a = eval(n->kids[0]), b = eval(n->kids[1]); return binop(n->s, a, b); }
        case Node::Member: {
            Val base = eval(n->kids[0]);
            std::string sw = n->s;
            if (sw.size() == 1) return Val::scalar(base.v[swizzle_index(sw[0])]);
            std::vector<float> comps; for (char c : sw) comps.push_back(base.v[swizzle_index(c)]);
            return make_vec(comps);
        }
        case Node::Index: { Val base = eval(n->kids[0]); Val idx = eval(n->kids[1]); int i = (int)idx.v[0];
                            if (base.mat) { Val col; col.n = 4; for (int k = 0; k < 4; k++) col.v[k] = base.v[i*4+k]; return col; }
                            return Val::scalar(base.v[i]); }
        case Node::Call: { std::vector<Val> args; for (auto& k : n->kids) args.push_back(eval(k)); return call(n->s, args); }
        case Node::Assign: { Val r = eval(n->kids[1]); assign(n->kids[0], r); return r; }
        default: return Val{};
    }
}

Val Interp::binop(const std::string& op, Val a, Val b) {
    // matrix * vector / matrix
    if (op == "*" && a.mat && !b.mat) {  // mat4 * vec4 (column-major)
        Val r; r.n = 4; for (int row = 0; row < 4; row++) { float s = 0; for (int k = 0; k < 4; k++) s += a.v[k*4+row] * b.v[k]; r.v[row] = s; } return r;
    }
    if (op == "*" && a.mat && b.mat) {
        Val r; r.mat = true; for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) { float s = 0; for (int k = 0; k < 4; k++) s += a.v[k*4+row]*b.v[c*4+k]; r.v[c*4+row] = s; } return r;
    }
    int n = std::max(a.n, b.n);
    bool as = a.n == 1, bs = b.n == 1;
    Val r; r.n = n;
    for (int i = 0; i < n; i++) {
        float x = as ? a.v[0] : a.v[i], y = bs ? b.v[0] : b.v[i];
        if (op == "+") r.v[i] = x + y; else if (op == "-") r.v[i] = x - y;
        else if (op == "*") r.v[i] = x * y; else if (op == "/") r.v[i] = y != 0 ? x / y : 0;
        else if (op == "<") r.v[i] = x < y; else if (op == ">") r.v[i] = x > y;
        else if (op == "<=") r.v[i] = x <= y; else if (op == ">=") r.v[i] = x >= y;
        else if (op == "==") r.v[i] = x == y; else if (op == "!=") r.v[i] = x != y;
        else if (op == "&&") r.v[i] = (x != 0 && y != 0); else if (op == "||") r.v[i] = (x != 0 || y != 0);
    }
    return r;
}

Val Interp::call(const std::string& name, std::vector<Val>& args) {
    auto comps = [&]() { std::vector<float> c; for (auto& a : args) for (int i = 0; i < (a.mat?16:a.n); i++) c.push_back(a.v[i]); return c; };
    if (name == "vec2" || name == "vec3" || name == "vec4") {
        int want = name == "vec2" ? 2 : name == "vec3" ? 3 : 4;
        std::vector<float> c = comps();
        if (c.size() == 1) c.assign(want, c[0]);  // vecN(scalar)
        c.resize(want, c.empty() ? 0 : c.back());
        return make_vec(c);
    }
    if (name == "mat4") { Val r; r.mat = true; std::vector<float> c = comps();
        if (c.size() == 1) { for (int i = 0; i < 4; i++) r.v[i*4+i] = c[0]; }  // diagonal
        else for (size_t i = 0; i < 16 && i < c.size(); i++) r.v[i] = c[i];
        return r; }
    if (name == "float") return Val::scalar(args.empty() ? 0 : args[0].v[0]);
    if (name == "int") return Val::scalar(args.empty() ? 0 : std::trunc(args[0].v[0]));
    auto u1 = [&](std::function<float(float)> f) { Val r = args[0]; for (int i = 0; i < r.n; i++) r.v[i] = f(args[0].v[i]); return r; };
    if (name == "abs") return u1([](float x){ return std::fabs(x); });
    if (name == "floor") return u1([](float x){ return std::floor(x); });
    if (name == "ceil") return u1([](float x){ return std::ceil(x); });
    if (name == "fract") return u1([](float x){ return x - std::floor(x); });
    if (name == "sin") return u1([](float x){ return std::sin(x); });
    if (name == "cos") return u1([](float x){ return std::cos(x); });
    if (name == "sqrt") return u1([](float x){ return std::sqrt(x); });
    if (name == "radians") return u1([](float x){ return x * 3.14159265f / 180.f; });
    if (name == "length") { float s = 0; for (int i = 0; i < args[0].n; i++) s += args[0].v[i]*args[0].v[i]; return Val::scalar(std::sqrt(s)); }
    if (name == "dot") { float s = 0; for (int i = 0; i < args[0].n; i++) s += args[0].v[i]*args[1].v[i]; return Val::scalar(s); }
    if (name == "normalize") { float s = 0; for (int i = 0; i < args[0].n; i++) s += args[0].v[i]*args[0].v[i]; s = std::sqrt(s); Val r = args[0]; if (s>0) for (int i=0;i<r.n;i++) r.v[i]/=s; return r; }
    if (name == "min") { Val r = args[0]; for (int i=0;i<r.n;i++) r.v[i] = std::min(args[0].v[i], args[1].n==1?args[1].v[0]:args[1].v[i]); return r; }
    if (name == "max") { Val r = args[0]; for (int i=0;i<r.n;i++) r.v[i] = std::max(args[0].v[i], args[1].n==1?args[1].v[0]:args[1].v[i]); return r; }
    if (name == "pow") { Val r = args[0]; for (int i=0;i<r.n;i++) r.v[i] = std::pow(args[0].v[i], args[1].n==1?args[1].v[0]:args[1].v[i]); return r; }
    if (name == "mod") { Val r = args[0]; for (int i=0;i<r.n;i++){ float m = args[1].n==1?args[1].v[0]:args[1].v[i]; r.v[i] = args[0].v[i] - std::floor(args[0].v[i]/m)*m; } return r; }
    if (name == "clamp") { Val r = args[0]; for (int i=0;i<r.n;i++){ float lo=args[1].n==1?args[1].v[0]:args[1].v[i], hi=args[2].n==1?args[2].v[0]:args[2].v[i]; r.v[i] = std::min(std::max(r.v[i],lo),hi);} return r; }
    if (name == "mix") { Val r = args[0]; for (int i=0;i<r.n;i++){ float t = args[2].n==1?args[2].v[0]:args[2].v[i]; r.v[i] = args[0].v[i]*(1-t) + args[1].v[i]*t; } return r; }
    if (name == "step") { Val r = args[1]; for (int i=0;i<r.n;i++){ float e=args[0].n==1?args[0].v[0]:args[0].v[i]; r.v[i] = args[1].v[i] < e ? 0 : 1; } return r; }
    if (name == "texture2D" || name == "texture") {
        if (sampler && *sampler && args.size() >= 2) return (*sampler)((int)args[0].v[0], args[1].v[0], args[1].v[1]);
        return make_vec({1,1,1,1});
    }
    return Val{};
}

}  // namespace

// ---- Program ----
struct ProgramImpl { NP root; };
Program::Program() {}
Program::~Program() {}

bool Program::compile(const std::string& src, std::string& log) {
    Parser ps(src);
    attributes_.clear(); varyings_.clear(); uniforms_.clear();
    root_ = ps.parse_program(attributes_, varyings_, uniforms_);
    if (!ps.err.empty()) { log = ps.err; return false; }
    return true;
}

bool Program::run(const std::map<std::string, Val>& inputs, std::map<std::string, Val>& out,
                  const Sampler& sampler) {
    if (!root_) return false;
    std::map<std::string, Val> env = inputs;
    // find main()
    NP main;
    for (auto& k : root_->kids) if (k->k == Node::Func && k->s == "main") main = k;
    if (!main) return false;
    Interp it(env);
    it.sampler = &sampler;
    it.exec(main->kids[0]);
    out = env;
    return true;
}

} // namespace malibu::shader
