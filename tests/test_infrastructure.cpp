// tests/test_infrastructure.cpp
// Placeholder test to verify test infrastructure works.

#include <gtest/gtest.h>

TEST(InfrastructureTest, BasicAssertions) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
    EXPECT_FALSE(false);
}

TEST(InfrastructureTest, StringComparison) {
    std::string a = "hello";
    std::string b = "world";
    EXPECT_NE(a, b);
    EXPECT_EQ(a, "hello");
}

TEST(InfrastructureTest, VectorOperations) {
    std::vector<int> v = {1, 2, 3};
    EXPECT_EQ(v.size(), 3);
    v.push_back(4);
    EXPECT_EQ(v.size(), 4);
    EXPECT_EQ(v[3], 4);
}

TEST(InfrastructureTest, NullPlatformRegistration) {
    // This test will be expanded when we have the platform headers available
    // For now, just verify the test framework works
    SUCCEED();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}