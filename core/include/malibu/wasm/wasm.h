#pragma once
// core/include/malibu/wasm/wasm.h
// MalibuWASM — a from-scratch WebAssembly engine (no Wasmtime/V8/Emscripten).
//
// Pipeline (each phase separated, per the architecture rule):
//   .wasm bytes -> binary decoder -> Module (sections/types/funcs/...) ->
//   validator -> interpreter (a pure stack machine).
//
// This library has NO dependency on the JS engine; the `WebAssembly` JS global
// is installed by a thin binding layer that calls this C++ API. Linear memory is
// a plain byte vector that the host (TypedArrays) can view.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>

namespace malibu::wasm {

enum class ValType : uint8_t { I32 = 0x7F, I64 = 0x7E, F32 = 0x7D, F64 = 0x7C, Void = 0x40 };

struct Value {
    ValType type = ValType::I32;
    union { int32_t i32; int64_t i64; float f32; double f64; };
    Value() : i32(0) {}
    static Value I32(int32_t v) { Value r; r.type = ValType::I32; r.i32 = v; return r; }
    static Value I64(int64_t v) { Value r; r.type = ValType::I64; r.i64 = v; return r; }
    static Value F32(float v)   { Value r; r.type = ValType::F32; r.f32 = v; return r; }
    static Value F64(double v)  { Value r; r.type = ValType::F64; r.f64 = v; return r; }
};

struct FuncType {
    std::vector<ValType> params;
    std::vector<ValType> results;
};

// A host (imported) function: receives args, returns results.
using HostFn = std::function<std::vector<Value>(const std::vector<Value>&)>;

struct Func {
    uint32_t             type_index = 0;
    std::vector<ValType> locals;     // declared locals (after params)
    std::vector<uint8_t> code;       // raw instruction bytes of the body
    bool                 is_import = false;
    HostFn               host;       // set when is_import
};

struct Export {
    std::string name;
    uint8_t     kind = 0;   // 0=func 1=table 2=mem 3=global
    uint32_t    index = 0;
};

struct Global {
    ValType type = ValType::I32;
    bool    mutable_ = false;
    Value   value;
};

struct Memory {
    std::vector<uint8_t> data;
    uint32_t             max_pages = 0;   // 0 == unlimited
    static constexpr uint32_t kPageSize = 65536;
};

// A decoded + validated module.
struct Module {
    std::vector<FuncType> types;
    std::vector<Func>     funcs;       // imported funcs first, then defined
    std::vector<Export>   exports;
    std::vector<Global>   globals;
    bool                  has_memory = false;
    uint32_t              mem_min_pages = 0;
    uint32_t              mem_max_pages = 0;
    std::vector<std::pair<std::string, std::string>> func_imports;  // (module, name) per imported func
    int                   start_func = -1;
};

struct DecodeResult {
    std::unique_ptr<Module> module;
    std::string             error;
    [[nodiscard]] bool ok() const { return module != nullptr; }
};

// Decode (and structurally validate) a .wasm binary.
DecodeResult decode(const uint8_t* bytes, size_t len);

// A runnable instance: the module plus its mutable state (memory, globals).
class Instance {
public:
    explicit Instance(const Module* m) : module_(m) {}

    const Module* module() const { return module_; }
    Memory&       memory() { return memory_; }
    std::vector<Global>& globals() { return globals_; }
    std::vector<Func>&   funcs() { return funcs_; }

    // Look up an exported function index by name (or -1).
    int export_func(const std::string& name) const;

    // Invoke function #index with args. Returns results, or an error string.
    std::optional<std::vector<Value>> invoke(uint32_t func_index,
                                             const std::vector<Value>& args,
                                             std::string& error);

private:
    const Module*        module_;
    Memory               memory_;
    std::vector<Global>  globals_;
    std::vector<Func>    funcs_;   // copy with imports' host fns bound
    friend struct Interp;
    friend std::unique_ptr<Instance> instantiate(
        const Module&, const std::vector<HostFn>&, std::string&);
};

// Instantiate a module. `host_funcs` provides one HostFn per imported function,
// in import order. Returns null + fills `error` on failure.
std::unique_ptr<Instance> instantiate(const Module& m,
                                      const std::vector<HostFn>& host_funcs,
                                      std::string& error);

} // namespace malibu::wasm
