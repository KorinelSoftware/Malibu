// tests/test_wasm.cpp — MalibuWASM decoder + interpreter (vertical slices).
#include <gtest/gtest.h>
#include "malibu/wasm/wasm.h"

using namespace malibu::wasm;

namespace {
// add(a,b) = a+b, exported as "add".
const std::vector<uint8_t> kAddModule = {
    0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,           // magic + version
    0x01,0x07,0x01, 0x60,0x02,0x7f,0x7f,0x01,0x7f,       // type: (i32,i32)->i32
    0x03,0x02,0x01,0x00,                                 // func: 1 func, type 0
    0x07,0x07,0x01, 0x03,'a','d','d', 0x00,0x00,         // export "add" -> func 0
    0x0a,0x09,0x01, 0x07,0x00, 0x20,0x00,0x20,0x01,0x6a,0x0b, // code: get0 get1 add end
};

std::optional<int32_t> run_i32(const std::vector<uint8_t>& bytes, const char* fn,
                               std::vector<Value> args) {
    auto dr = decode(bytes.data(), bytes.size());
    if (!dr.ok()) return std::nullopt;
    std::string err;
    auto inst = instantiate(*dr.module, {}, err);
    if (!inst) return std::nullopt;
    int fi = inst->export_func(fn);
    if (fi < 0) return std::nullopt;
    auto rets = inst->invoke(static_cast<uint32_t>(fi), args, err);
    if (!rets || rets->empty()) return std::nullopt;
    return (*rets)[0].i32;
}
}  // namespace

TEST(MalibuWASM, Slice1_AddTwoNumbers) {
    auto r = run_i32(kAddModule, "add", {Value::I32(2), Value::I32(3)});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 5);
    EXPECT_EQ(*run_i32(kAddModule, "add", {Value::I32(40), Value::I32(2)}), 42);
    EXPECT_EQ(*run_i32(kAddModule, "add", {Value::I32(-10), Value::I32(7)}), -3);
}

TEST(MalibuWASM, DecodeRejectsGarbage) {
    std::vector<uint8_t> junk = {0x01, 0x02, 0x03, 0x04};
    EXPECT_FALSE(decode(junk.data(), junk.size()).ok());
}

// fac(n): iterative factorial using a loop + br_if (exercises control flow).
//   (func (param i32) (result i32) (local i32 i32)
//      i32.const 1; local.set 1            ; acc = 1
//      i32.const 1; local.set 2            ; i = 1
//      block
//        loop
//          local.get 2; local.get 0; i32.gt_s; br_if 1   ; if i>n break
//          local.get 1; local.get 2; i32.mul; local.set 1 ; acc *= i
//          local.get 2; i32.const 1; i32.add; local.set 2 ; i++
//          br 0
//        end
//      end
//      local.get 1)
TEST(MalibuWASM, ControlFlow_Factorial) {
    std::vector<uint8_t> body = {
        0x02,0x02,                                       // 2 locals decls? no — encoded below
    };
    // Build the code body precisely.
    std::vector<uint8_t> code = {
        0x41,0x01, 0x21,0x01,                            // i32.const 1; local.set 1
        0x41,0x01, 0x21,0x02,                            // i32.const 1; local.set 2
        0x02,0x40,                                       // block void
          0x03,0x40,                                     // loop void
            0x20,0x02, 0x20,0x00, 0x4a, 0x0d,0x01,       // get2 get0 gt_s br_if 1
            0x20,0x01, 0x20,0x02, 0x6c, 0x21,0x01,       // get1 get2 mul set1
            0x20,0x02, 0x41,0x01, 0x6a, 0x21,0x02,       // get2 const1 add set2
            0x0c,0x00,                                   // br 0
          0x0b,                                          // end loop
        0x0b,                                            // end block
        0x20,0x01,                                       // local.get 1 (result)
        0x0b,                                            // end func
    };
    // locals: 2 i32 locals -> "01 02 7f" (1 decl group, count 2, i32)
    std::vector<uint8_t> locals = {0x01, 0x02, 0x7f};
    std::vector<uint8_t> fnbody;
    fnbody.insert(fnbody.end(), locals.begin(), locals.end());
    fnbody.insert(fnbody.end(), code.begin(), code.end());

    std::vector<uint8_t> mod = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x01,0x06,0x01, 0x60,0x01,0x7f,0x01,0x7f,        // type (i32)->i32
        0x03,0x02,0x01,0x00,                             // func type 0
        0x07,0x07,0x01, 0x03,'f','a','c', 0x00,0x00,     // export "fac"
        0x0a,                                            // code section id
    };
    mod.push_back(static_cast<uint8_t>(fnbody.size() + 2));  // section size
    mod.push_back(0x01);                                     // 1 body
    mod.push_back(static_cast<uint8_t>(fnbody.size()));      // body size
    mod.insert(mod.end(), fnbody.begin(), fnbody.end());
    (void)body;

    EXPECT_EQ(*run_i32(mod, "fac", {Value::I32(5)}), 120);
    EXPECT_EQ(*run_i32(mod, "fac", {Value::I32(0)}), 1);
    EXPECT_EQ(*run_i32(mod, "fac", {Value::I32(6)}), 720);
}
