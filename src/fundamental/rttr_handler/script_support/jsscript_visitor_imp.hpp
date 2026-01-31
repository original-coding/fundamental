#pragma once

#include "fundamental/basic/type_traits.hpp"

#include <iostream>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#include "quickjs/quickjs-libc.h"
#include "quickjspp.hpp"
#pragma GCC diagnostic pop
#define SUPPORT_JS__REF
namespace Fundamental
{
template <typename T>
struct support_script_access<std::vector<T>> : std::true_type {};

template <typename T>
struct support_script_access<std::list<T>> : std::true_type {};

template <typename T>
struct support_script_access<std::set<T>> : std::true_type {};

template <typename T>
struct support_script_access<std::unordered_set<T>> : std::true_type {};

template <typename K, typename V>
struct support_script_access<std::map<K, V>> : std::true_type {};
template <typename K, typename V>
struct support_script_access<std::unordered_map<K, V>> : std::true_type {};
} // namespace Fundamental
namespace qjs
{
template <typename T, typename = void>
struct select_js_type {
    using type = std::decay_t<T>;
};
template <class T>
struct select_js_type<
    T,
    std::enable_if_t<std::is_class_v<std::decay_t<T>> && Fundamental::is_script_base_v<std::decay_t<T>>>> {
    using type = T;
};

template <class T>
using select_js_type_t = typename select_js_type<T>::type;

template <class T>
struct js_traits<T&,
                 std::enable_if_t<std::is_class_v<std::decay_t<T>> && Fundamental::is_script_base_v<std::decay_t<T>>>> {
    using raw_type = std::decay_t<T>;
    static JSValue wrap(JSContext* ctx, T& value) {
        if (js_traits<std::shared_ptr<raw_type>>::QJSClassId == 0) // not registered
        {
#if defined(__cpp_rtti)
            // If you have an error here with T=JSValueConst
            // it probably means you are passing JSValueConst to where JSValue is expected
            js_traits<std::shared_ptr<raw_type>>::register_class(ctx, typeid(T).name());
#else
            JS_ThrowTypeError(ctx, "quickjspp js_traits<const T&>::wrap: Class is not registered");
            return JS_EXCEPTION;
#endif
        }
        auto jsobj = JS_NewObjectClass(ctx, js_traits<std::shared_ptr<raw_type>>::QJSClassId);
        if (JS_IsException(jsobj)) return jsobj;
        // shared_ptr with empty deleter since we don't own T*
        auto pptr = new std::shared_ptr<raw_type>(const_cast<raw_type*>(&value), [](raw_type*) {});
        JS_SetOpaque(jsobj, pptr);
        return jsobj;
    }

    static T& unwrap(JSContext* ctx, JSValueConst v) {
        if (JS_IsNull(v)) {
            throw exception { ctx };
        }
        auto ptr = js_traits<std::shared_ptr<raw_type>>::unwrap(ctx, v);
        if (!ptr) {
            throw exception { ctx };
        }
        return *ptr;
    }
};

template <class T>
struct js_traits<T, std::enable_if_t<std::is_class_v<T> && Fundamental::is_script_base_v<T>>> {
    static JSValue wrap(JSContext* ctx, T value) {
        if (js_traits<std::shared_ptr<T>>::QJSClassId == 0) // not registered
        {
#if defined(__cpp_rtti)
            // If you have an error here with T=JSValueConst
            // it probably means you are passing JSValueConst to where JSValue is expected
            js_traits<std::shared_ptr<T>>::register_class(ctx, typeid(T).name());
#else
            JS_ThrowTypeError(ctx, "quickjspp js_traits<const T&>::wrap: Class is not registered");
            return JS_EXCEPTION;
#endif
        }
        auto jsobj = JS_NewObjectClass(ctx, js_traits<std::shared_ptr<T>>::QJSClassId);
        if (JS_IsException(jsobj)) return jsobj;
        // shared_ptr with empty deleter since we don't own T*
        auto pptr = new std::shared_ptr<T>(new T(std::forward<T>(value)));
        JS_SetOpaque(jsobj, pptr);
        return jsobj;
    }

    static T unwrap(JSContext* ctx, JSValueConst v) {
        if (JS_IsNull(v)) {
            throw exception { ctx };
        }
        auto ptr = js_traits<std::shared_ptr<T>>::unwrap(ctx, v);
        if (!ptr) {
            throw exception { ctx };
        }
        return *ptr;
    }
};

template <typename R, typename... Args, typename Callable>
JSValue wrap_call_forward(JSContext* ctx, Callable&& f, int argc, JSValueConst* argv) noexcept {
    try {
        if constexpr (std::is_same_v<R, void>) {
            std::apply(std::forward<Callable>(f), detail::unwrap_args<Args...>(ctx, argc, argv));
            return JS_NULL;
        } else {
            return js_traits<select_js_type_t<R>>::wrap(
                ctx, std::apply(std::forward<Callable>(f), detail::unwrap_args<Args...>(ctx, argc, argv)));
        }
    } catch (exception) {
        return JS_EXCEPTION;
    } catch (std::exception const& err) {
        JS_ThrowInternalError(ctx, "%s", err.what());
        return JS_EXCEPTION;
    } catch (...) {
        JS_ThrowInternalError(ctx, "Unknown error");
        return JS_EXCEPTION;
    }
}

/** Same as wrap_call, but pass this_value as first argument.
 * @tparam FirstArg type of this_value
 */
template <typename R, typename FirstArg, typename... Args, typename Callable>
JSValue wrap_this_call_forward(JSContext* ctx,
                               Callable&& f,
                               JSValueConst this_value,
                               int argc,
                               JSValueConst* argv) noexcept {
    try {
        if constexpr (std::is_same_v<R, void>) {
            std::apply(std::forward<Callable>(f), std::tuple_cat(detail::unwrap_args<FirstArg>(ctx, 1, &this_value),
                                                                 detail::unwrap_args<Args...>(ctx, argc, argv)));
            return JS_NULL;
        } else {
            return js_traits<select_js_type_t<R>>::wrap(
                ctx,
                std::apply(std::forward<Callable>(f), std::tuple_cat(detail::unwrap_args<FirstArg>(ctx, 1, &this_value),
                                                                     detail::unwrap_args<Args...>(ctx, argc, argv))));
        }
    } catch (exception) {
        return JS_EXCEPTION;
    } catch (std::exception const& err) {
        JS_ThrowInternalError(ctx, "%s", err.what());
        return JS_EXCEPTION;
    } catch (...) {
        JS_ThrowInternalError(ctx, "Unknown error");
        return JS_EXCEPTION;
    }
}

template <typename MemFunc>
auto make_js_member_callable(MemFunc func);

template <typename ClassType, typename ReturnType, typename... Args>
auto make_js_member_callable(ReturnType (ClassType::*func)(Args...)) {
    return [func](std::shared_ptr<ClassType> obj, Args&&... args) -> ReturnType {
        return (obj.get()->*func)(std::forward<Args>(args)...);
    };
}

template <typename ClassType, typename ReturnType, typename... Args>
auto make_js_member_callable(ReturnType (ClassType::*func)(Args...) const) {
    return [func](std::shared_ptr<ClassType> obj, Args&&... args) -> ReturnType {
        return (obj.get()->*func)(std::forward<Args>(args)...);
    };
}

struct js_magic_func_wrap {
    using magic_func_t = std::function<JSValue(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)>;

    template <typename Func>
    static JSValue wrap(JSContext* ctx, const char* name, Func&& func) {
        using func_type   = typename Fundamental::function_traits<std::decay_t<Func>>::stl_function_type;
        func_type functor = func;

        return wrap_imp(ctx, name, functor);
    }
    template <typename ClassT, typename Func>
    static JSValue wrap_class(JSContext* ctx, const char* name, Func&& func) {
        using func_type   = typename Fundamental::function_traits<std::decay_t<Func>>::stl_function_type;
        func_type functor = func;

        return wrap_this_imp<ClassT>(ctx, name, functor);
    }
    template <typename R, typename... Args>
    static JSValue wrap_imp(JSContext* ctx, const char* name, const std::function<R(Args...)>& func) {
        auto magic     = static_cast<std::int32_t>(registered_funcs.size());
        auto& new_func = registered_funcs.emplace_back();
        new_func       = [func = func](JSContext* ctx, JSValueConst, int argc,
                                 JSValueConst* argv) mutable noexcept -> JSValue {
#ifndef SUPPORT_JS__REF
            return detail::wrap_call<R, Args...>(ctx, func, argc, argv);
#else
            return wrap_call_forward<R, Args...>(ctx, func, argc, argv);
#endif
        };
        return JS_NewCFunctionMagic(ctx, &js_magic_func_wrap::magic_func_call, name, sizeof...(Args),
                                    JSCFunctionEnum::JS_CFUNC_generic_magic, magic);
    }
    template <typename ClassT, typename R, typename... Args>
    static JSValue wrap_this_imp(JSContext* ctx, const char* name, const std::function<R(Args...)>& func) {
        auto magic     = static_cast<std::int32_t>(registered_funcs.size());
        auto& new_func = registered_funcs.emplace_back();
        new_func       = [func = func](JSContext* ctx, JSValueConst this_val, int argc,
                                 JSValueConst* argv) mutable noexcept -> JSValue {
#ifndef SUPPORT_JS__REF
            return detail::wrap_this_call<R, Args...>(ctx, func, this_val, argc, argv);
#else
            return wrap_this_call_forward<R, Args...>(ctx, func, this_val, argc, argv);
#endif
        };
        return JS_NewCFunctionMagic(ctx, &js_magic_func_wrap::magic_func_call, name, sizeof...(Args) - 1,
                                    JSCFunctionEnum::JS_CFUNC_generic_magic, magic);
    }

    static JSValue magic_func_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
        auto magic_offset = static_cast<std::size_t>(magic);
        if (magic_offset >= registered_funcs.size()) {
            return JS_EXCEPTION;
        }
        auto& func = registered_funcs[magic_offset];
        if (!func) {
            return JS_EXCEPTION;
        }
        return func(ctx, this_val, argc, argv);
    }
    inline static std::vector<magic_func_t> registered_funcs;
};

} // namespace qjs