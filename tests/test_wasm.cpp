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

std::vector<uint8_t> memory_module(std::vector<uint8_t> code) {
    std::vector<uint8_t> body = {0x00};  // no locals
    body.insert(body.end(), code.begin(), code.end());
    std::vector<uint8_t> module = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x01,0x04,0x01, 0x60,0x00,0x00,             // type: () -> ()
        0x03,0x02,0x01,0x00,                         // one function
        0x05,0x03,0x01,0x00,0x01,                    // one-page memory
        0x07,0x07,0x01, 0x03,'r','u','n',0x00,0x00,  // export function
        0x0a,
    };
    module.push_back(static_cast<uint8_t>(body.size() + 2));
    module.push_back(0x01);
    module.push_back(static_cast<uint8_t>(body.size()));
    module.insert(module.end(), body.begin(), body.end());
    return module;
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

TEST(MalibuWASM, DecodesExportedReferenceTableLimits) {
    const std::vector<uint8_t> module = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x04,0x05,0x01, 0x6f,0x01,0x02,0x04,
        0x07,0x09,0x01, 0x05,'t','a','b','l','e', 0x01,0x00,
    };
    auto decoded = decode(module.data(), module.size());
    ASSERT_TRUE(decoded.ok()) << decoded.error;
    ASSERT_EQ(decoded.module->tables.size(), 1u);
    EXPECT_EQ(decoded.module->tables[0].element_type, 0x6f);
    EXPECT_EQ(decoded.module->tables[0].min_size, 2u);
    EXPECT_TRUE(decoded.module->tables[0].has_max);
    EXPECT_EQ(decoded.module->tables[0].max_size, 4u);
    ASSERT_EQ(decoded.module->exports.size(), 1u);
    EXPECT_EQ(decoded.module->exports[0].kind, 1);
    EXPECT_EQ(decoded.module->exports[0].index, 0u);
}

TEST(MalibuWASM, ExecutesMvpMemoryLoadsAndStores) {
    std::vector<uint8_t> code = {
        0x41,0x00, 0x43,0x00,0x00,0x60,0x40, 0x38,0x02,0x00,
        0x41,0x08, 0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x1d,0xc0,
        0x39,0x03,0x00,
        0x41,0x10, 0x42,0x2a, 0x37,0x03,0x00,
        0x41,0x18, 0x41,0x7f, 0x3b,0x01,0x00,
        0x41,0x20, 0x42,0x7f, 0x3e,0x02,0x00,
        0x41,0x00, 0x2a,0x02,0x00, 0x1a,
        0x41,0x08, 0x2b,0x03,0x00, 0x1a,
        0x41,0x10, 0x29,0x03,0x00, 0x1a,
        0x41,0x18, 0x2e,0x01,0x00, 0x1a,
        0x41,0x20, 0x35,0x02,0x00, 0x1a,
        0x0b,
    };
    auto bytes = memory_module(std::move(code));
    auto decoded = decode(bytes.data(), bytes.size());
    ASSERT_TRUE(decoded.ok()) << decoded.error;
    std::string error;
    auto instance = instantiate(*decoded.module, {}, error);
    ASSERT_NE(instance, nullptr) << error;
    int function = instance->export_func("run");
    ASSERT_GE(function, 0);
    auto result = instance->invoke(static_cast<uint32_t>(function), {}, error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_TRUE(result->empty());

    const auto& memory = instance->memory().data;
    EXPECT_EQ(memory[0], 0x00);
    EXPECT_EQ(memory[1], 0x00);
    EXPECT_EQ(memory[2], 0x60);
    EXPECT_EQ(memory[3], 0x40);
    EXPECT_EQ(memory[8], 0x00);
    EXPECT_EQ(memory[14], 0x1d);
    EXPECT_EQ(memory[15], 0xc0);
    EXPECT_EQ(memory[16], 42);
    EXPECT_EQ(memory[24], 0xff);
    EXPECT_EQ(memory[25], 0xff);
    EXPECT_EQ(memory[32], 0xff);
    EXPECT_EQ(memory[35], 0xff);
}

TEST(MalibuWASM, AppliesActiveDataSegmentsDuringInstantiation) {
    const std::vector<uint8_t> module = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x05,0x03,0x01,0x00,0x01,
        0x0b,0x09,0x01,0x00,0x41,0x04,0x0b,0x03,'a','b','c',
    };
    auto decoded = decode(module.data(), module.size());
    ASSERT_TRUE(decoded.ok()) << decoded.error;
    ASSERT_EQ(decoded.module->data_segments.size(), 1u);
    std::string error;
    auto instance = instantiate(*decoded.module, {}, error);
    ASSERT_NE(instance, nullptr) << error;
    ASSERT_GE(instance->memory().data.size(), 7u);
    EXPECT_EQ(instance->memory().data[3], 0);
    EXPECT_EQ(instance->memory().data[4], 'a');
    EXPECT_EQ(instance->memory().data[5], 'b');
    EXPECT_EQ(instance->memory().data[6], 'c');
}

TEST(MalibuWASM, DispatchesCallIndirectThroughActiveElementSegment) {
    const std::vector<uint8_t> module = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
        0x03,0x03,0x02,0x00,0x00,
        0x04,0x04,0x01,0x70,0x00,0x01,
        0x07,0x07,0x01,0x03,'r','u','n',0x00,0x01,
        0x09,0x07,0x01,0x00,0x41,0x00,0x0b,0x01,0x00,
        0x0a,0x0e,0x02,
        0x04,0x00,0x41,0x2a,0x0b,
        0x07,0x00,0x41,0x00,0x11,0x00,0x00,0x0b,
    };
    auto result = run_i32(module, "run", {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(MalibuWASM, ReferenceNullReportsAsNull) {
    const std::vector<uint8_t> module = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
        0x03,0x02,0x01,0x00,
        0x07,0x0a,0x01,0x06,'i','s','N','u','l','l',0x00,0x00,
        0x0a,0x07,0x01,0x05,0x00,0xd0,0x6f,0xd1,0x0b,
    };
    auto result = run_i32(module, "isNull", {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

TEST(MalibuWASM, BranchTableSelectsNestedLabelDepth) {
    const std::vector<uint8_t> module = {
        0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
        0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
        0x03,0x02,0x01,0x00,
        0x07,0x0a,0x01,0x06,'b','r','a','n','c','h',0x00,0x00,
        0x0a,0x1c,0x01,0x1a,
        0x01,0x01,0x7f,
        0x41,0x09,0x21,0x01,
        0x02,0x40,
          0x02,0x40,
            0x20,0x00,0x0e,0x01,0x00,0x01,
          0x0b,
          0x41,0x01,0x21,0x01,
        0x0b,
        0x20,0x01,0x0b,
    };
    EXPECT_EQ(*run_i32(module, "branch", {Value::I32(0)}), 1);
    EXPECT_EQ(*run_i32(module, "branch", {Value::I32(1)}), 9);
    EXPECT_EQ(*run_i32(module, "branch", {Value::I32(20)}), 9);
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
