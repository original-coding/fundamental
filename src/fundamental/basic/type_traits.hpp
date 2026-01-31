#pragma once
#include <functional>
#include <tuple>
#include <type_traits>

namespace Fundamental
{

template <typename T>
struct support_script_access : std::false_type {};

struct script_base_type {};

template <typename T, typename Tp = std::decay_t<T>>
inline constexpr bool is_script_base_v = std::is_base_of_v<script_base_type, Tp> || support_script_access<Tp>::value;

template <typename T>
struct function_traits;

template <typename Ret, typename... Args>
struct function_traits<Ret(Args...)> {
public:
    static constexpr size_t arity = sizeof...(Args);

    using function_type     = Ret(Args...);
    using return_type       = Ret;
    using stl_function_type = std::function<function_type>;
    using args_tuple        = std::tuple<Args...>;

    static constexpr bool is_void       = std::is_void_v<Ret>;
    static constexpr bool returns_value = !is_void;

    template <size_t N>
    using arg_type = typename std::tuple_element<N, args_tuple>::type;
};

template <typename Ret>
struct function_traits<Ret()> {
public:
    static constexpr size_t arity = 0;

    using function_type     = Ret();
    using return_type       = Ret;
    using stl_function_type = std::function<function_type>;
    using args_tuple        = std::tuple<>;

    static constexpr bool is_void       = std::is_void_v<Ret>;
    static constexpr bool returns_value = !is_void;

    template <size_t N>
    using arg_type = void;
};

template <typename Ret, typename... Args>
struct function_traits<Ret(Args...) noexcept> : function_traits<Ret(Args...)> {};

template <typename Ret>
struct function_traits<Ret() noexcept> : function_traits<Ret()> {};

template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...)> : function_traits<Ret(Args...)> {};

template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...) noexcept> : function_traits<Ret(Args...) noexcept> {};

template <typename Ret, typename... Args>
struct function_traits<std::function<Ret(Args...)>> : function_traits<Ret(Args...)> {};

#define FUNCTION_TRAITS_MEMBER(QUALIFIERS)                                                                             \
    template <typename Ret, typename Class, typename... Args>                                                          \
    struct function_traits<Ret (Class::*)(Args...) QUALIFIERS> : function_traits<Ret(Args...)> {                       \
        using class_type = Class;                                                                                      \
    };

FUNCTION_TRAITS_MEMBER()
FUNCTION_TRAITS_MEMBER(const)
FUNCTION_TRAITS_MEMBER(volatile)
FUNCTION_TRAITS_MEMBER(const volatile)
FUNCTION_TRAITS_MEMBER(noexcept)
FUNCTION_TRAITS_MEMBER(const noexcept)
FUNCTION_TRAITS_MEMBER(volatile noexcept)
FUNCTION_TRAITS_MEMBER(const volatile noexcept)

#undef FUNCTION_TRAITS_MEMBER

template <typename Callable>
struct function_traits : function_traits<decltype(&Callable::operator())> {};



template<typename T>
struct member_pointer_traits;

template<typename Class, typename Type>
struct member_pointer_traits<Type Class::*> {
    using class_type = Class;
    using member_type = Type;
};
} // namespace Fundamental