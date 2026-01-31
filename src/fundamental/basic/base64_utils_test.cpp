#include "fundamental/basic/base64_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/basic/utils.hpp"

int main(int argc, char* argv[]) {
    using ValueType = std::uint8_t;
    ValueType kMin  = 1;
    ValueType kMax  = 127;
    auto generator  = Fundamental::DefaultNumberGenerator(kMin, kMax);
    using InputType = std::vector<std::uint8_t>;
    InputType input;

    input.resize(100);
    generator.gen(reinterpret_cast<std::uint8_t*>(&input[0]), input.size());
    FINFO("input:{}", Fundamental::Utils::BufferToHex(input.data(), input.size()));
    {
        constexpr auto coderType = Fundamental::Base64CoderType::kFSBase64;
        std::string output       = Fundamental::Base64Encode<coderType>(input.data(), input.size());
        FINFO("output:{}", output);
        InputType decodeOutput;

        Fundamental::Base64Decode<coderType>(output, decodeOutput);
        FASSERT_ACTION(input.size() == decodeOutput.size() &&
                           std::memcmp(input.data(), decodeOutput.data(), input.size()) == 0,
                       std::abort());
    }
    {
        constexpr auto coderType = Fundamental::Base64CoderType::kNormalBase64;
        std::string output       = Fundamental::Base64Encode<coderType>(input.data(), input.size());
        FINFO("output:{}", output);
        InputType decodeOutput;
        Fundamental::Base64Decode<coderType>(output, decodeOutput);
        FASSERT_ACTION(input.size() == decodeOutput.size() &&
                           std::memcmp(input.data(), decodeOutput.data(), input.size()) == 0,
                       std::abort());
    }
    {
        std::size_t max_test_nums = 1000;
        std::string output;
        output.resize((max_test_nums + 2) / 3 * 4);
        std::size_t current_encode_size = 0;
        for (std::size_t count = 1; count < max_test_nums; ++count) {
            input.resize(count);
            generator.gen(reinterpret_cast<std::uint8_t*>(&input[0]), input.size());
            Fundamental::Base64Encode(input.data(), input.size(), output.data(), output.size(), current_encode_size);
            InputType decodeOutput;
            decodeOutput.resize(count);
            std::size_t out_size = 0;
            Fundamental::Base64Decode(output.data(), current_encode_size, decodeOutput.data(), decodeOutput.size(),
                                      out_size);
            FASSERT_ACTION(out_size == count && input.size() == decodeOutput.size() &&
                               std::memcmp(input.data(), decodeOutput.data(), input.size()) == 0,
                           std::abort());
        }
    }

    {
        struct TestCase {
            std::string input;
            std::string success_expected;
            bool should_succeed;
        };

        std::vector<TestCase> test_cases = {
            // success
            { "SGVsbG8gV29ybGQh", "Hello World!", true },
            { "SGVsbG8gV29ybGQ=", "Hello World", true },
            { "SGVsbG8gV29ybA==", "Hello Worl", true },
            { "QQ==", "A", true },
            { "QUI=", "AB", true },
            { "QUJD", "ABC", true },

            // failed
            { "", "", false },
            { "QQ=", "", false },
            { "QQ======", "", false },
            { "QQ=A", "", false },
            { "QQ!=", "", false },
            { "A===", "", false },
        };

        for (const auto& test : test_cases) {
            std::string decodeOutput;
            auto ret = Fundamental::Base64Decode(test.input, decodeOutput);
            FASSERT_ACTION(ret == test.should_succeed &&
                               (!test.should_succeed || decodeOutput == test.success_expected),
                           std::abort(), "{} {}=={} {}=={}", test.input, ret, test.should_succeed, decodeOutput,
                           test.success_expected);
        }
    }
    return 0;
}
