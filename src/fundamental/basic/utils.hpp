#ifndef _HEAD_BASIC_UTILS_
#define _HEAD_BASIC_UTILS_
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#define F_UNUSED(x) (void)x

namespace Fundamental
{

template <auto>
inline constexpr bool always_false = false;

struct NonCopyable {
    NonCopyable()                              = default;
    NonCopyable(const NonCopyable&)            = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

struct NonMovable {
    NonMovable()                         = default;
    NonMovable(NonMovable&&)             = delete;
    NonMovable& operator=(NonCopyable&&) = delete;
};

using BasicTaskFunctionT = std::function<void()>;
struct ScopeGuard final : NonCopyable {
    ScopeGuard() = default;
    ScopeGuard(const BasicTaskFunctionT& _f, const BasicTaskFunctionT& initF = nullptr) : f(_f) {
        if (initF) initF();
    }

    ~ScopeGuard() {
        execute();
    }

    ScopeGuard(ScopeGuard&& other) noexcept : f(std::move(other.f)) {
    }

    ScopeGuard& operator=(ScopeGuard&& other) noexcept {
        reset(std::move(other.f));
        return *this;
    }

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ScopeGuard>(std::forward<Args>(args)...);
    }

    void reset(const BasicTaskFunctionT& _f) {
        execute();
        f = _f;
    }

    void dismiss() {
        f = nullptr;
    }

    void execute() {
        if (f) f();
        f = nullptr;
    }

private:
    BasicTaskFunctionT f = nullptr;
};

template <typename T>
struct Singleton : NonCopyable, NonMovable {
    static T& Instance() {
        // notice we can't use a value type here
        // sometimes we will access another static instance to release some runtime resources
        //  or access a logger instance
        //  when we access another static instance in the desturct function,
        // The memory access safety of this behavior cannot be guaranteed
        static T* instance = new T();
        return *instance;
    };

protected:
    Singleton() {
    }
    ~Singleton() {
    }
};

template <std::size_t N>
struct PowerOfTwo {
    static constexpr std::size_t value = 2 * PowerOfTwo<N - 1>::value;
};

template <>
struct PowerOfTwo<0> {
    static constexpr std::size_t value = 1;
};

namespace Utils
{
using fpid_t = std::uint32_t;
fpid_t GetProcessId();
void SetThreadName(const std::string& name);
std::string BufferToHex(const void* buffer, std::size_t size, std::size_t group_size = 0, std::int8_t spilt_char = 0);
template <typename T, typename = std::void_t<typename T::value_type>>
inline std::string BufferToHex(const T& v, std::size_t group_size = 0, std::int8_t spilt_char = 0) {
    using ValueType = typename T::value_type;
    return BufferToHex(v.data(), v.size() * sizeof(ValueType), group_size, spilt_char);
}

std::string BufferDumpAscii(const void* buffer, std::size_t size);
struct NetworkInfo {
    std::string ifName;
    std::string ipv4;
    std::string mac;
    bool isLoopback = false;
};
std::unordered_map<std::string, NetworkInfo> GetLocalNetInformation();
[[nodiscard]] std::string RemoveComments(std::string_view input);
} // namespace Utils

} // namespace Fundamental
#endif // _HEAD_BASIC_UTILS_