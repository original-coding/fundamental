
#include "fundamental/basic/log.h"
#include <gtest/gtest.h>

#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/basic/utils.hpp"

// Demonstrate some basic assertions.
TEST(TestFundamentalAll, basic_filesystem_utils) {
    auto test_file_name = "test.file";
    Fundamental::ScopeGuard g([&]() {
        try {
            std_fs::remove(test_file_name);
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
        }
    });
    auto gen = Fundamental::DefaultNumberGenerator<char>('a', 'z');
    std::string test_data;
    test_data.resize(1024);
    gen.gen(test_data.data(), test_data.size());
    {
        // write
        EXPECT_TRUE(Fundamental::fs::WriteFile(test_file_name, test_data.data(), test_data.size()));
        auto part_size = test_data.size() / 2;
        // read
        {
            std::string part;
            EXPECT_TRUE(Fundamental::fs::ReadFile(test_file_name, part, 0, part_size));
            EXPECT_TRUE(part_size == part.size());
            EXPECT_TRUE(part == test_data.substr(0, part_size));
        }
        {
            std::string part;
            EXPECT_TRUE(Fundamental::fs::ReadFile(test_file_name, part, part_size, part_size));
            EXPECT_TRUE(part_size == part.size());
            EXPECT_TRUE(part == test_data.substr(part_size, part_size));
        }
    }
}

int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    FDEBUG("start test");
    return RUN_ALL_TESTS();
}