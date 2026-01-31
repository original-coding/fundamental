#pragma once
#include <exception>
#include <string>
#include <string_view>
#include <system_error>

namespace Fundamental
{

using error_category = std::error_category;

enum class basic_errors : std::int32_t
{
    basic_success = 0,
    basic_failed  = 1,
};

class basic_category : public std::error_category {
public:
    static const basic_category& Instance() {
        static basic_category instance;
        return instance;
    }

    const char* name() const noexcept override {
        return "basic.opcode";
    }
    
    std::string message(int value) const override {
        switch (static_cast<basic_errors>(value)) {
        case basic_errors::basic_success: return "success";
        case basic_errors::basic_failed: return "failed";
        default: return std::string("basic error ") + std::to_string(value);
        }
    }
};

class error_code : public std::error_code {
public:
    error_code() noexcept : std::error_code() {
    }

    error_code(int __v, const error_category& __cat) noexcept : std::error_code(__v, __cat) {
    }

    error_code(int __v, const error_category& __cat, std::string_view details) :
    std::error_code(__v, __cat), details__(details) {
    }

    error_code(const error_code& other) :
    std::error_code(other.value(), other.category()), details__(other.details_view()) {
    }

    error_code(error_code&& other) noexcept :
    std::error_code(other.value(), other.category()), details__(std::move(other.details__)) {
    }

    error_code(const std::error_code& other, std::string_view details = "") :
    std::error_code(other), details__(details) {
    }

    error_code(std::error_code&& other, std::string_view details = "") noexcept :
    std::error_code(std::move(other)), details__(std::move(details)) {
    }

    static error_code make_basic_error(int __v = 0, std::string_view details = "") {
        return error_code(__v, basic_category::Instance(), details);
    }

    error_code& operator=(const error_code& other) {
        std::error_code::assign(other.value(), other.category());
        details__ = other.details__;
        return *this;
    }
    error_code& operator=(error_code&& other) noexcept {
        std::error_code::assign(other.value(), other.category());
        other.clear();
        details__ = std::move(other.details__);
        return *this;
    }

    error_code& operator=(const std::error_code& other) {
        std::error_code::assign(other.value(), other.category());
        return *this;
    }

    error_code& operator=(std::error_code&& other) noexcept {
        std::error_code::assign(other.value(), other.category());
        other.clear();
        return *this;
    }

    error_code& assign(int v, const std::error_category& cat, std::string_view details) noexcept {
        std::error_code::assign(v, cat);
        details__ = details;
        return *this;
    }

    error_code& assign_details(std::string_view details) noexcept {
        details__ = details;
        return *this;
    }

    decltype(auto) make_excepiton() const noexcept {
        return std::system_error(*static_cast<const std::error_code*>(this), details__);
    }
    decltype(auto) make_exception_ptr() const {
        return std::make_exception_ptr(make_excepiton());
    }

    const char* details_c_str() const noexcept {
        return details__.c_str();
    }

    const std::string& details() const noexcept {
        return details__;
    }

    std::string_view details_view() const noexcept {
        return details__;
    }

    std::string full_message() const noexcept {
        auto msg = message();
        if (msg.empty()) {
            return details();
        }
        if (details__.empty()) {
            return msg;
        }
        return msg + ":" + details__;
    }

private:
    std::string details__;
};

template <typename T>
inline error_code make_error_code(T e, const error_category& __cat, std::string_view details = {}) {
    return error_code(static_cast<std::int32_t>(e), __cat, details);
}

inline error_code make_system_code(std::string_view details = {}) {
    return error_code(errno, std::system_category(), details);
}

inline error_code make_system_code(std::errc code, std::string_view details = {}) {
    return error_code(static_cast<std::int32_t>(code), std::system_category(), details);
}
} // namespace Fundamental