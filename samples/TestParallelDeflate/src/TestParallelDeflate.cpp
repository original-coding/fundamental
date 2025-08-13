
#ifndef FORCE_TIME_TRACKER
    #define FORCE_TIME_TRACKER 1
#endif

#include "fundamental/basic/compress_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/basic/utils.hpp"
#include "fundamental/thread_pool/thread_pool.h"
#include "fundamental/tracker/time_tracker.hpp"
#include <fstream>

#include <cmath>
#include <gtest/gtest.h>
#include <list>
#include <vector>
using namespace Fundamental;
Fundamental::ThreadPoolParallelExecutor s_excutor;
TEST(deflate_test, test_basic) {
    auto random_generator = Fundamental::DefaultNumberGenerator<std::size_t>(1000000, 2000000);
    auto test_data_cnt    = random_generator();
    std::vector<std::size_t> input_data;
    input_data.resize(test_data_cnt);
    random_generator.gen(input_data.data(), input_data.size());
    std::size_t input_len = sizeof(std::size_t) * test_data_cnt;
    Fundamental::DeflateConfig config;
    std::size_t output_need_size = config.GuessCompressLen(input_len);
    std::vector<std::uint8_t> dst_buf;
    dst_buf.resize(output_need_size);
    std::string tag = Fundamental::StringFormat("size={}", input_len);

    { // test normal deflate
        std::vector<std::uint8_t> decompress_dst_buf;
        decompress_dst_buf.resize(input_len);
        std::size_t compress_size   = output_need_size;
        std::size_t decompress_size = input_len;
        {
            using Type = STimeTracker<std::chrono::milliseconds>;
            DeclareTimeTacker(Type, t, tag, "normal deflate", 10, true, nullptr);
            bool ret = Fundamental::ZUtils::DeflateBinary(input_data.data(), input_len, dst_buf.data(), &compress_size);
            EXPECT_TRUE(ret);
        }
        {
            using Type = STimeTracker<std::chrono::milliseconds>;
            DeclareTimeTacker(Type, t, tag, "normal inflate", 10, true, nullptr);
            bool ret = Fundamental::ZUtils::InflateBinary(dst_buf.data(), compress_size, decompress_dst_buf.data(),
                                                          &decompress_size);
            EXPECT_TRUE(ret);
        }
        EXPECT_TRUE(decompress_size == input_len &&
                    std::memcmp(decompress_dst_buf.data(), input_data.data(), input_len) == 0);
    }
    { // test parallel deflate
        std::vector<std::uint8_t> decompress_dst_buf;
        decompress_dst_buf.resize(input_len);
        std::size_t compress_size   = output_need_size;
        std::size_t decompress_size = input_len;
        {
            using Type = STimeTracker<std::chrono::milliseconds>;
            DeclareTimeTacker(Type, t, tag, "parallel deflate", 10, true, nullptr);
            auto [ret, _] = Fundamental::ZUtils::ParallelDeflateBinary(input_data.data(), input_len, dst_buf.data(),
                                                                       &compress_size, s_excutor, config);

            EXPECT_TRUE(ret);
        }
        {
            using Type = STimeTracker<std::chrono::milliseconds>;
            DeclareTimeTacker(Type, t, tag, "normal inflate", 10, true, nullptr);
            bool ret = Fundamental::ZUtils::InflateBinary(dst_buf.data(), compress_size, decompress_dst_buf.data(),
                                                          &decompress_size);
            EXPECT_TRUE(ret);
        }
        EXPECT_TRUE(decompress_size == input_len &&
                    std::memcmp(decompress_dst_buf.data(), input_data.data(), input_len) == 0);
    }
    { // test parallel deflate
        std::vector<std::uint8_t> decompress_dst_buf;
        decompress_dst_buf.resize(input_len);
        std::size_t compress_size   = output_need_size;
        std::size_t decompress_size = input_len;
        config.output_format        = DeflateOutputFormat::RAW_DEFLATE_STREAM_FORMAT;
        config.check_sum_type       = DeflateCheckSumType::CRC32_CHECK_T;
        {
            using Type = STimeTracker<std::chrono::milliseconds>;
            DeclareTimeTacker(Type, t, tag, "parallel raw deflate", 10, true, nullptr);
            auto [ret, _] = Fundamental::ZUtils::ParallelDeflateBinary(input_data.data(), input_len, dst_buf.data(),
                                                                       &compress_size, s_excutor, config);
            EXPECT_TRUE(ret);
        }
        {
            using Type = STimeTracker<std::chrono::milliseconds>;
            DeclareTimeTacker(Type, t, tag, "normal inflate", 10, true, nullptr);
            bool ret = Fundamental::ZUtils::InflateBinary(dst_buf.data(), compress_size, decompress_dst_buf.data(),
                                                          &decompress_size);
            EXPECT_FALSE(ret);
        }
        EXPECT_FALSE(decompress_size == input_len &&
                     std::memcmp(decompress_dst_buf.data(), input_data.data(), input_len) == 0);
    }
    {
        std::string zip_data(random_generator.gen(), 'c');
        std::size_t compress_size = output_need_size;
        auto [ret, check_sum]     = Fundamental::ZUtils::ParallelDeflateBinary(
            zip_data.data(), zip_data.size(), dst_buf.data(), &compress_size, s_excutor, config);
        {
            Fundamental::ZipWriter writer;

            writer.AddFile("1", dst_buf.data(), compress_size, zip_data.size(), check_sum);
            writer.AddFile("2/test", dst_buf.data(), compress_size, zip_data.size(), check_sum);
            writer.AddFile("3", dst_buf.data(), compress_size, zip_data.size(), check_sum);
            auto zip_ret = std::move(writer.Filnalize());
            EXPECT_THROW(writer.AddFile("test2.binary", dst_buf.data(), compress_size, zip_data.size(), check_sum),
                         std::runtime_error);
            EXPECT_GT(zip_ret.size(), compress_size);
            std::fstream out_stream;
            out_stream.open("out.zip", std::ios::binary | std::ios::out);
            out_stream.write((char*)zip_ret.data(), zip_ret.size());
            EXPECT_TRUE(!out_stream.fail());
        }
    }
}

int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}