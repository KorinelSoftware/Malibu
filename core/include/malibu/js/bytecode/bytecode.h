#pragma once
// core/include/malibu/js/bytecode/bytecode.h
// Bytecode format, instruction encoding, and CallSiteTable (Task 15).
//
// The bytecode is a flat array of fixed-width 64-bit instructions. The format
// header carries a monotonic version integer; the runtime refuses to execute
// bytecode whose version differs from the current engine version.

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace malibu::js::bytecode {

// "MLBS" little-endian magic.
inline constexpr uint32_t kBytecodeMagic   = 0x4D4C4253u;
// Monotonic version — increment on ANY change to the instruction set / encoding.
inline constexpr uint32_t kBytecodeVersion = 4u;

struct BytecodeHeader {
    uint32_t magic              = kBytecodeMagic;
    uint32_t version            = kBytecodeVersion;
    uint32_t constant_pool_size = 0;
    uint32_t register_count     = 0;
    uint32_t instruction_count  = 0;
};

enum class OpCode : uint8_t {
    Nop, LoadConst, LoadGlobal, StoreGlobal,
    LoadLocal, StoreLocal, LoadArg, StoreArg,
    Add, Sub, Mul, Div, Mod, Neg,
    BitAnd, BitOr, BitXor, Shl, Shr, UShr,
    Eq, NEq, Lt, Lte, Gt, Gte, StrictEq, StrictNEq,
    LogAnd, LogOr, LogNot,
    Jump, JumpIfTrue, JumpIfFalse,
    Call, CallMethod, TailCall,
    Return, ReturnUndefined,
    NewObject, NewArray, NewClosure,
    GetProp, SetProp, GetElem, SetElem, DeleteProp,
    GetSuperProp, SetSuperProp, GetSuperElem, SetSuperElem,
    TypeOf, InstanceOf, In,
    Throw, TryCatch, TryFinally, EndTry,
    Await, Yield, YieldStar,
    WebCall,
    DebugBreak,

    // --- Engine instruction set (Task 14/15/17 full compiler + interpreter) ---
    Move,            // dst <- src_a
    LoadString,      // dst <- JSString(str_consts[imm])
    LoadUndefined,   // dst <- undefined
    LoadNull,        // dst <- null
    LoadBool,        // dst <- (imm != 0)
    LoadThis,        // dst <- this
    DefineVar,       // dst: 0 current, 1 function, 2 function-if-absent
    LoadVar,         // dst <- env.lookup(str_consts[imm])
    StoreVar,        // env.assign(str_consts[imm], reg[src_a])
    PushScope,       // enter a child lexical scope
    PopScope,        // leave the current scope
    Pow,             // dst <- src_a ** src_b
    BitNot,          // dst <- ~src_a
    GetLength,       // dst <- length(src_a)
    ToIterable,      // dst <- array of values(imm=0) / keys(imm=1) of src_a
    PushHandler,     // dst=flags(b0 catch,b1 finally) src_a=finallyPC src_b=excReg imm=catchPC
    PopHandler,      // discard the top exception handler
    EndFinally,      // rethrow the pending exception if one is in flight
    Construct,       // dst <- new callee(args): src_a=base reg, imm=argc
    SetProto,        // internal [[Prototype]] of reg[dst] <- reg[src_a]
    CallV,           // dst <- callee(this, ...argsArray): src_a=calleeReg src_b=thisReg imm=argsArrayReg
    ConstructV,      // dst <- new callee(...argsArray): src_a=calleeReg imm=argsArrayReg
    ArrayAppend,     // reg[dst] <- value (imm=0), spread (imm=1), or an elision/hole (imm=2)
    CopyProps,       // copy own enumerable props of reg[src_a] into reg[dst] (object spread)
    DefineAccessor,  // accessor; src_b bit 0=setter, bit 1=enumerable
    DefineAccessorV, // computed accessor; imm bit 0=setter, bit 1=enumerable
    SetFnName,       // if reg[dst] is a function with an empty name, name <- str_consts[imm]
    DeleteElem,      // dst <- delete reg[src_a][reg[src_b]]
    PushWithScope,   // enter a `with` scope backed by the object in reg[src_a]
    LoadVarOrUndef,  // dst <- env.lookup(str_consts[imm]) or undefined (no throw) — for `typeof x`
    LoadBigInt,      // dst <- JSBigInt(bigint_consts[imm])
    Inc,             // dst <- ToNumeric(src_a) + same-type one
    Dec,             // dst <- ToNumeric(src_a) - same-type one
};

// ---------------------------------------------------------------------------
// Instruction encoding (64-bit fixed width):
//   bits[63:56] = opcode  (uint8_t)
//   bits[55:48] = dst_reg (uint8_t)
//   bits[47:32] = src_a   (uint16_t)
//   bits[31:16] = src_b   (uint16_t)
//   bits[15:0]  = imm16   (int16_t)
//
// WebCall specialization (so the VM can locate its CallSiteEntry):
//   dst_reg = result register
//   src_a   = receiver register
//   src_b   = first-argument register (arguments are contiguous)
//   imm16   = call_site_id (looked up in CallSiteTable to find webcall_id)
// ---------------------------------------------------------------------------
struct Instruction {
    OpCode   op;
    uint8_t  dst;
    uint16_t src_a;
    uint16_t src_b;
    int16_t  imm16;
};

[[nodiscard]] constexpr uint64_t encode(OpCode op, uint8_t dst, uint16_t src_a,
                                        uint16_t src_b, int16_t imm16) noexcept {
    return (static_cast<uint64_t>(static_cast<uint8_t>(op)) << 56) |
           (static_cast<uint64_t>(dst) << 48) |
           (static_cast<uint64_t>(src_a) << 32) |
           (static_cast<uint64_t>(src_b) << 16) |
           (static_cast<uint64_t>(static_cast<uint16_t>(imm16)));
}

[[nodiscard]] constexpr Instruction decode(uint64_t raw) noexcept {
    return Instruction{
        static_cast<OpCode>(static_cast<uint8_t>(raw >> 56)),
        static_cast<uint8_t>((raw >> 48) & 0xFF),
        static_cast<uint16_t>((raw >> 32) & 0xFFFF),
        static_cast<uint16_t>((raw >> 16) & 0xFFFF),
        static_cast<int16_t>(static_cast<uint16_t>(raw & 0xFFFF)),
    };
}

// ---------------------------------------------------------------------------
// CallSiteEntry / CallSiteTable
//
// One entry per WebCall opcode in a function's bytecode. Deoptimization is
// tracked here as metadata; bytecode is NEVER mutated in place.
// ---------------------------------------------------------------------------
struct CallSiteEntry {
    uint32_t call_site_id = 0;
    uint32_t webcall_id   = 0;
    bool     deoptimized  = false;
    uint64_t deopt_epoch  = 0;
};

class CallSiteTable {
public:
    void register_function(uint32_t func_id, const std::vector<CallSiteEntry>& sites);

    // Sets deoptimized=true for the site and bumps its epoch. Idempotent-safe.
    void deoptimize(uint32_t call_site_id);

    // Clears deoptimized for the site (re-enables the fast path).
    void reoptimize(uint32_t call_site_id);

    [[nodiscard]] bool is_deoptimized(uint32_t call_site_id) const;

    // Returns the webcall_id for a site, or UINT32_MAX if unknown.
    [[nodiscard]] uint32_t webcall_id_of(uint32_t call_site_id) const;

    // Current deopt epoch for a site (0 if unknown / never deoptimized).
    [[nodiscard]] uint64_t epoch_of(uint32_t call_site_id) const;

private:
    mutable std::mutex                              mu_;
    std::unordered_map<uint32_t, CallSiteEntry>     sites_;
};

// ---------------------------------------------------------------------------
// A compiled function: header + flat instruction stream + call-site metadata.
// ---------------------------------------------------------------------------
struct BytecodeFunction {
    BytecodeHeader             header;
    std::vector<uint64_t>      code;        // encoded instructions
    std::vector<CallSiteEntry> call_sites;  // one per WebCall site

    [[nodiscard]] bool version_ok() const noexcept {
        return header.magic == kBytecodeMagic && header.version == kBytecodeVersion;
    }
};

} // namespace malibu::js::bytecode
