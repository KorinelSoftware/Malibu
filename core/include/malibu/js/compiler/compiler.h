#pragma once
// core/include/malibu/js/compiler/compiler.h
// AST -> bytecode compiler (Task 15). Produces a tree of Function templates the
// interpreter instantiates into closures. Registers are scratch for expression
// evaluation; named variables live in lexical Environments (Task 17).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "malibu/js/vm/value.h"
#include "malibu/js/parser/parser.h"

namespace malibu::js::compiler {

// A compiled function template: bytecode + pools + nested functions.
struct Function {
    std::u16string                          name;
    uint32_t                                arity = 0;
    std::vector<std::u16string>             param_names;
    bool                                    is_arrow = false;
    bool                                    is_async = false;
    bool                                    is_generator = false;

    uint32_t                                num_registers = 0;
    std::vector<uint64_t>                   code;            // encoded instructions
    std::vector<vm::Value>                  num_consts;      // numeric/bool constants
    std::vector<std::u16string>             str_consts;      // strings, var/property names
    std::vector<std::shared_ptr<Function>>  functions;       // nested function templates
};

class Compiler {
public:
    struct Result {
        std::shared_ptr<Function> function;   // null on error
        std::string               error;      // empty on success
        [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    };

    // Compiles a parsed Program node into a top-level Function.
    Result compile(const parser::Node& program);
};

} // namespace malibu::js::compiler
