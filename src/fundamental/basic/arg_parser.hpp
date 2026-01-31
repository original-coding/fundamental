#pragma once

#include <cctype>

#include <getopt.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Fundamental
{

// to support custom type, you can specialize this template in namespace Fundamental like below
// or support the following constructor T(const std::string&)
template <typename T>
inline T from_string(const std::string& v) {
    return T(v);
}

class arg_parser {
public:
    enum param_type : std::int32_t
    {
        with_none_param,
        required_param,
        optional_param
    };
    struct parser_entry {
        std::string name;
        std::int32_t option_value;
        param_type type;
        std::string opt_usage;
        std::string param_description;
        std::optional<std::vector<std::optional<std::string>>> parser_values;
    };
    static constexpr std::int8_t kReservedShortOption[] = { '?' /*for invalid option*/, ':' /*missing param*/ };
    static constexpr std::int32_t kNoShortOption        = -1;
    static constexpr std::int32_t kHealperShortOption   = 'h';
    static constexpr std::int32_t kVersionShortOption   = 'v';
    static constexpr const char* kHelperOptionName      = "help";
    static constexpr const char* kVersionOptionName     = "version";

public:
    arg_parser(int argc,
               char* argv[],
               const std::string& version    = "1.0.0",
               const std::string& build_date = __DATE__,
               const std::string& build_time = __TIME__);
    ~arg_parser();
    bool AddOption(const std::string& name,
                   const std::string& opt_usage,
                   std::int32_t short_option_value      = kNoShortOption,
                   param_type type                      = with_none_param,
                   const std::string& param_description = "param");
    // return false only means some fatal error occurs,you can see the error message in stderr
    bool ParseCommandLine();
    void ShowHelp();
    void ShowVersion();
    std::optional<std::vector<std::optional<std::string>>> GetOptionValues(const std::string& name) const;
    std::optional<std::string> GetOptionValue(const std::string& name) const;
    const std::vector<std::string>& GetNonOptionValues() const;
    bool HasParam(const std::string& name = kHelperOptionName) const;
    void DumpOptions() const;

    template <typename T>
    T GetValue(const std::string& name, const T& default_value = {}) const {
        auto v = GetOptionValue(name);
        if (!v.has_value()) return default_value;
        try {
            return from_string<T>(v.value());
        } catch (const std::exception& e) {
            std::cerr << "convert option:" << name << " value:" << v.value() << " failed:" << e.what() << std::endl;
            return default_value;
        }
    }

    template <typename T>
    std::vector<T> GetValues(const std::string& name, const T& default_value = {}) const {
        auto v = GetOptionValues(name);
        if (!v.has_value()) return {};
        auto values = v.value();
        std::vector<T> ret;
        for (const auto& value : values) {
            try {
                if (value.has_value()) {
                    ret.push_back(from_string<T>(value.value()));
                } else {
                    ret.emplace_back(default_value);
                }
            } catch (const std::exception& e) {
                std::cerr << "convert option:" << name << " value:" << value.value_or("[-default-]")
                          << " failed:" << e.what() << std::endl;
            }
        }
        return ret;
    }

private:
    const int argc;
    char** const argv;
    const std::string version;
    const std::string build_date;
    const std::string build_time;
    std::int32_t current_option_value = 128;
    std::string_view program_path;
    std::vector<std::string> no_option_params;
    std::unordered_map<std::string, parser_entry> options;
    std::map<std::int32_t, std::string> short_options_dict;
};

inline arg_parser::arg_parser(int argc,
                              char* argv[],
                              const std::string& version,
                              const std::string& build_date,
                              const std::string& build_time) :
argc(argc), argv(argv), version(version), build_date(build_date), build_time(build_time), program_path(argv[0]) {
    AddOption(kHelperOptionName, "show this help page", kHealperShortOption, with_none_param);
    AddOption(kVersionOptionName, "show version information", kVersionShortOption, with_none_param);
    auto size = sizeof(kReservedShortOption);
    for (size_t i = 0; i < size; i++) {
        short_options_dict.emplace(kReservedShortOption[i], "__reserved__");
    }
}

inline arg_parser::~arg_parser() {
}

inline bool arg_parser::AddOption(const std::string& name,
                                  const std::string& opt_usage,
                                  std::int32_t option_value,
                                  param_type type,
                                  const std::string& param_description) {
    auto iter = options.find(name);
    if (iter != options.end()) {
        std::cerr << "option " << name << " already exists." << std::endl;
        return false;
    }
    if (option_value != kNoShortOption && !std::isprint(option_value)) {
        std::cerr << "Invalid short option value " << option_value << std::endl;
        return false;
    }
    auto iter_short = short_options_dict.find(option_value);
    if (iter_short != short_options_dict.end()) {
        std::cerr << "short option value:" << (char)option_value << " already exists for option:" << iter_short->second
                  << std::endl;
        return false;
    }
    if (option_value == kNoShortOption) {
        option_value = current_option_value++;
    }
    short_options_dict.emplace(option_value, name);
    options.emplace(name, parser_entry { name, option_value, type, opt_usage, param_description, std::nullopt });
    return true;
}

inline bool arg_parser::ParseCommandLine() {

    std::vector<option> long_options;
    std::string short_options;
    for (const auto& [name, option] : options) {
        auto& new_long_option = long_options.emplace_back();
        new_long_option.name  = option.name.c_str();
        std::string new_short_option;
        if (std::isprint(option.option_value)) {
            new_short_option.push_back(static_cast<char>(option.option_value));
        }
        if (option.type == with_none_param) {
            new_long_option.has_arg = no_argument;
        } else if (option.type == required_param) {
            new_long_option.has_arg = required_argument;
            if (!new_short_option.empty()) new_short_option += ":";
        } else {
            new_long_option.has_arg = optional_argument;
            if (!new_short_option.empty()) new_short_option += "::";
        }
        new_long_option.flag = nullptr;
        new_long_option.val  = option.option_value;
        short_options += new_short_option;
    }
    long_options.push_back({ nullptr, 0, nullptr, 0 });
    short_options.push_back('\0');
    while (true) {
        int option_index = 0;
        int res          = getopt_long(argc, argv, short_options.data(), long_options.data(), &option_index);

        if (res == -1) {
            break;
        }

        switch (res) {
        case 0: {
            std::cerr << "internal error for flag long opt" << std::endl;
            return false;
        }
        case '?': std::cerr << "invalid option: -" << static_cast<char>(optopt) << std::endl; break;
        case ':': std::cerr << "option -" << optopt << " requires an argument" << std::endl; return false;
        default: {
            auto iter = short_options_dict.find(res);
            if (iter == short_options_dict.end()) {
                std::cerr << "internal error can't find:" << res << std::endl;
                return false;
            }
            auto option_iter = options.find(iter->second);
            if (option_iter == options.end()) {
                std::cerr << "internal error can't find option:" << iter->second << std::endl;
                return false;
            }

            auto& current_option = option_iter->second;
            if (!current_option.parser_values.has_value()) {
                current_option.parser_values = std::vector<std::optional<std::string>>();
            }
            current_option.parser_values.value().emplace_back();
            switch (current_option.type) {
            case optional_param: {
                if (optarg) {
                    current_option.parser_values.value().back() = optarg;
                }

            } break;
            case required_param: {
                if (!optarg) {
                    std::cerr << "paser " << iter->second << " internal error" << std::endl;
                    return false;
                }
                current_option.parser_values.value().back() = optarg;
            } break;

            default: break;
            }
        }
        }
    }
    while (optind < argc) {
        no_option_params.push_back(argv[optind]);
        ++optind;
    }
    return true;
}

inline void arg_parser::ShowHelp() {
    std::cout << "Usage for program:" << program_path << " version:" << version << std::endl;
    std::cout << build_date << " " << build_time << " built" << std::endl;
    std::cout << "Notice:For options with optional arguments,the argument must either immediately follow the short "
                 "option or be connected with =."
              << std::endl;
    std::cout << "For example: -a[arg]  or --long-option[=arg]" << std::endl;
    for (const auto& [name, option] : options) {
        if (option.option_value < 128) {
            std::cout << "\t-" << (char)(option.option_value) << ",";
        } else {
            std::cout << "\t";
        }
        std::cout << "--" << name;
        if (option.type == required_param) {
            std::cout << " <" << option.param_description << ">";
        } else if (option.type == optional_param) {
            std::cout << " [" << option.param_description << "]";
        }
        std::cout << "\t" << option.opt_usage << std::endl;
    }
}

inline std::optional<std::vector<std::optional<std::string>>> arg_parser::GetOptionValues(
    const std::string& name) const {
    auto iter = options.find(name);
    return iter == options.end() ? std::nullopt : iter->second.parser_values;
}

inline std::optional<std::string> arg_parser::GetOptionValue(const std::string& name) const {
    auto iter = options.find(name);
    if (iter == options.end() || !iter->second.parser_values.has_value()) return std::nullopt;
    auto& value = iter->second.parser_values.value();
    if (value.empty()) return std::nullopt;
    return value[0];
}

inline const std::vector<std::string>& arg_parser::GetNonOptionValues() const {
    return no_option_params;
}

inline bool arg_parser::HasParam(const std::string& name) const {
    return GetOptionValues(name).has_value();
}

inline void arg_parser::DumpOptions() const {
    std::cout << "Dump all parsered args for program:" << program_path << std::endl;
    for (const auto& [name, option] : options) {
        if (option.option_value < 128) {
            std::cout << "-" << (char)(option.option_value) << ",";
        }
        std::cout << "--" << name;
        std::cout << "\t";
        if (option.parser_values.has_value())
            std::cout << "{set}";
        else
            std::cout << "{not set}";
        if (option.type == required_param) {
            if (option.parser_values.has_value()) {
                std::cout << " <";
                auto& values = option.parser_values.value();
                for (size_t i = 0; i < values.size(); i++) {
                    if (i != 0) {
                        std::cout << " ";
                    }
                    std::cout << values[i].value_or("[internal error]");
                }
                std::cout << ">";
            }
        } else if (option.type == optional_param) {
            if (option.parser_values.has_value()) {
                std::cout << " [";
                auto& values = option.parser_values.value();
                for (size_t i = 0; i < values.size(); i++) {
                    if (i != 0) {
                        std::cout << " ";
                    }
                    std::cout << values[i].value_or("[-default-]");
                }
                std::cout << "]";
            }
        }
        std::cout << std::endl;
    }
    std::cout << "Dump non option params:";
    for (size_t i = 0; i < no_option_params.size(); i++) {
        if (i != 0) {
            std::cout << " ";
        }

        std::cout << no_option_params[i];
    }
    std::cout << std::endl;
}

inline void arg_parser::ShowVersion() {
    std::cout << "Version: " << version << std::endl;
}

template <typename T>
inline std::string to_lower(
    const T& str_) { // both std::string and std::basic_string_view<char> (for magic_enum) are using to_lower
    std::string str(str_.size(), '\0');
    std::transform(str_.begin(), str_.end(), str.begin(),
                   [](char input) -> char { return static_cast<char>(::tolower(input)); });
    return str;
}

template <>
inline std::string from_string(const std::string& v) {
    return v;
}
template <>
inline char from_string(const std::string& v) {
    return v.empty()      ? throw std::invalid_argument("empty string")
           : v.size() > 1 ? v.substr(0, 2) == "0x" ? (char)std::stoul(v, nullptr, 16) : (char)std::stoi(v)
                          : v[0];
}
template <>
inline int from_string(const std::string& v) {
    return std::stoi(v);
}
template <>
inline short from_string(const std::string& v) {
    return static_cast<short>(std::stoi(v));
}
template <>
inline long from_string(const std::string& v) {
    return std::stol(v);
}
template <>
inline long long from_string(const std::string& v) {
    return std::stol(v);
}

template <>
inline bool from_string(const std::string& v) {
    return to_lower(v) == "true" || v == "1";
}

template <>
inline float from_string(const std::string& v) {
    return std::stof(v);
}

template <>
inline double from_string(const std::string& v) {
    return std::stod(v);
}
template <>
inline unsigned char from_string(const std::string& v) {
    return from_string<char>(v);
}
template <>
inline unsigned int from_string(const std::string& v) {
    return static_cast<unsigned int>(std::stoul(v));
}
template <>
inline unsigned short from_string(const std::string& v) {
    return static_cast<unsigned short>(std::stoul(v));
}
template <>
inline unsigned long from_string(const std::string& v) {
    return std::stoul(v);
}
template <>
inline unsigned long long from_string(const std::string& v) {
    return std::stoul(v);
}

} // namespace Fundamental