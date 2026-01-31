#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"
int main(int argc, char* argv[]) {
    Fundamental::arg_parser arg_parser { argc, argv, GIT_COMMIT_INFO };
    arg_parser.AddOption("verbose", "dump all options", 'V');
    arg_parser.AddOption("aa", "test short option no param", 'a');
    arg_parser.AddOption("bb", "test short option with required param", 'b',
                         Fundamental::arg_parser::param_type::required_param, "testparam");
    arg_parser.AddOption("cc", "test short option with optional param", 'c',
                         Fundamental::arg_parser::param_type::optional_param, "testoparam");
    arg_parser.AddOption("opts", "test read opt string", 'o', Fundamental::arg_parser::param_type::optional_param,
                         "testoparam");
    FASSERT(!arg_parser.AddOption("cc", "test short option with optional param", 'i',
                                  Fundamental::arg_parser::param_type::optional_param, "testoparam"));
    FASSERT(!arg_parser.AddOption("dd", "test short option with optional param", 'a',
                                  Fundamental::arg_parser::param_type::optional_param, "testoparam"));
    FASSERT(!arg_parser.AddOption("dd", "test short option with optional param", '?',
                                  Fundamental::arg_parser::param_type::optional_param, "testoparam"));
    FASSERT(!arg_parser.AddOption("dd", "test short option with optional param", ':',
                                  Fundamental::arg_parser::param_type::optional_param, "testoparam"));
    FASSERT(!arg_parser.AddOption("dd", "test short option with optional param", 'h',
                                  Fundamental::arg_parser::param_type::optional_param, "testoparam"));
    FASSERT(!arg_parser.AddOption("dd", "test short option with optional param", 'v',
                                  Fundamental::arg_parser::param_type::optional_param, "testoparam"));
    if (argc == 1) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (arg_parser.ParseCommandLine()) {
        FINFO("paser success");
    } else {
        FERR("paser failed");
    }
    if (arg_parser.HasParam()) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (arg_parser.HasParam(Fundamental::arg_parser::kVersionOptionName)) {
        arg_parser.ShowVersion();
        return 1;
    }
    if (arg_parser.HasParam("verbose")) {
        arg_parser.DumpOptions();
    }
    if (arg_parser.HasParam("opts")) {
        std::cout << "load:" << arg_parser.GetValue<std::string>("opts", "default_value") << std::endl;
        return 1;
    }
    {
        float v = arg_parser.GetValue<float>("bb", 1.1f);
        std::cout << "float v:" << v << std::endl;
    }
    {
        bool v = arg_parser.GetValue<bool>("bb", false);
        std::cout << "bool v:" << (v ? "true" : "false") << std::endl;
    }
    {
        std::string v = arg_parser.GetValue<std::string>("bb", "default_str");
        std::cout << "string v:" << v << std::endl;
    }
    {
        auto v = arg_parser.GetValues<std::int32_t>("cc");
        for (const auto& value : v) {
            std::cout << "cc:" << value << std::endl;
        }
    }

    return 0;
}
