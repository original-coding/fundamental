#pragma once

#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "sqlite.hpp"

namespace sqlite
{

namespace ext
{
namespace internal
{

template <class R, class... Ps>
void functionx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values);
template <class T, class... Ps>
void stepx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values);

template <class T>
void finishN_impl(sqlite3_context* ctx);
void function_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values);

void step_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values);

void finalize_impl(sqlite3_context* ctx);
} // namespace internal

class context : noncopyable {
public:
    explicit context(sqlite3_context* ctx, int nargs = 0, sqlite3_value** values = nullptr);

    int args_count() const;
    int args_bytes(int idx) const;
    int args_type(int idx) const;

    template <class T>
    T get(int idx) const {
        return get(idx, T());
    }

    void result(int value);
    void result(double value);
    void result(long long int value);
    void result(std::string const& value);
    void result(std::string_view value);
    void result(char const* value, bool fcopy);
    void result(void const* value, int n, bool fcopy);
    void result();
    void result(null_type);
    void result_copy(int idx);
    void result_error(char const* msg);

    void* aggregate_data(int size);
    int aggregate_count();

    template <class... Ts>
    std::tuple<Ts...> to_tuple() {
        return to_tuple_impl(0, *this, std::tuple<Ts...>());
    }

private:
    int get(int idx, int) const;
    double get(int idx, double) const;
    long long int get(int idx, long long int) const;
    char const* get(int idx, char const*) const;
    std::string get(int idx, std::string) const;
    void const* get(int idx, void const*) const;

    template <class H, class... Ts>
    static inline std::tuple<H, Ts...> to_tuple_impl(int index, const context& c, std::tuple<H, Ts...>&&) {
        auto h = std::make_tuple(c.context::get<H>(index));
        return std::tuple_cat(h, to_tuple_impl(++index, c, std::tuple<Ts...>()));
    }
    static inline std::tuple<> to_tuple_impl(int /*index*/, const context& /*c*/, std::tuple<>&&) {
        return std::tuple<>();
    }

private:
    sqlite3_context* ctx_;
    int nargs_;
    sqlite3_value** values_;
};
class function : noncopyable {
public:
    using function_handler = std::function<void(context&)>;
    using pfunction_base   = std::shared_ptr<void>;

    explicit function(database& db);

    int create(char const* name, function_handler h, int nargs = 0);

    template <class F>
    int create(char const* name, std::function<F> h) {
        fh_[name] = std::shared_ptr<void>(new std::function<F>(h));
        return create_function_impl<F>()(db_, fh_[name].get(), name);
    }

private:
    template <class R, class... Ps>
    struct create_function_impl;

    template <class R, class... Ps>
    struct create_function_impl<R(Ps...)> {
        int operator()(sqlite3* db, void* fh, char const* name) {
            return sqlite3_create_function(db, name, sizeof...(Ps), SQLITE_UTF8, fh, internal::functionx_impl<R, Ps...>,
                                           nullptr, nullptr);
        }
    };

private:
    sqlite3* db_;

    std::map<std::string, pfunction_base> fh_;
};
class aggregate : noncopyable {
public:
    using function_handler = std::function<void(context&)>;
    using pfunction_base   = std::shared_ptr<void>;

    explicit aggregate(database& db);

    int create(char const* name, function_handler s, function_handler f, int nargs = 1);

    template <class T, class... Ps>
    int create(char const* name);

private:
    sqlite3* db_;

    std::map<std::string, std::pair<pfunction_base, pfunction_base>> ah_;
};

namespace internal
{

template <size_t N>
struct Apply {
    template <typename F, typename T, typename... A>
    static inline auto apply(F&& f, T&& t, A&&... a)
        -> decltype(Apply<N - 1>::apply(std::forward<F>(f),
                                        std::forward<T>(t),
                                        std::get<N - 1>(std::forward<T>(t)),
                                        std::forward<A>(a)...)) {
        return Apply<N - 1>::apply(std::forward<F>(f), std::forward<T>(t), std::get<N - 1>(std::forward<T>(t)),
                                   std::forward<A>(a)...);
    }
};

template <>
struct Apply<0> {
    template <typename F, typename T, typename... A>
    static inline auto apply(F&& f, T&&, A&&... a) -> decltype(std::forward<F>(f)(std::forward<A>(a)...)) {

        return std::forward<F>(f)(std::forward<A>(a)...);
    }
};

template <typename F, typename T>
inline auto apply_f(F&& f, T&& t)
    -> decltype(Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(std::forward<F>(f),
                                                                                   std::forward<T>(t))) {
    return Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(std::forward<F>(f), std::forward<T>(t));
}

template <class R, class... Ps>
inline void functionx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values) {
    context c(ctx, nargs, values);
    auto f = static_cast<std::function<R(Ps...)>*>(sqlite3_user_data(ctx));
    c.result(apply_f(*f, c.to_tuple<Ps...>()));
}

template <class T, class... Ps>
inline void stepx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values) {
    context c(ctx, nargs, values);
    T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
    if (c.aggregate_count() == 1) new (t) T;
    apply_f([](T* tt, Ps... ps) { tt->step(ps...); }, std::tuple_cat(std::make_tuple(t), c.to_tuple<Ps...>()));
}

template <class T>
inline void finishN_impl(sqlite3_context* ctx) {
    context c(ctx);
    T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
    c.result(t->finish());
    t->~T();
}
inline void function_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values) {
    auto f = static_cast<function::function_handler*>(sqlite3_user_data(ctx));
    context c(ctx, nargs, values);
    (*f)(c);
}

inline void step_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values) {
    auto p = static_cast<std::pair<aggregate::pfunction_base, aggregate::pfunction_base>*>(sqlite3_user_data(ctx));
    auto s = static_cast<aggregate::function_handler*>((*p).first.get());
    context c(ctx, nargs, values);
    ((function::function_handler&)*s)(c);
}

inline void finalize_impl(sqlite3_context* ctx) {
    auto p = static_cast<std::pair<aggregate::pfunction_base, aggregate::pfunction_base>*>(sqlite3_user_data(ctx));
    auto f = static_cast<aggregate::function_handler*>((*p).second.get());
    context c(ctx);
    ((function::function_handler&)*f)(c);
}
} // namespace internal

inline context::context(sqlite3_context* ctx, int nargs, sqlite3_value** values) :
ctx_(ctx), nargs_(nargs), values_(values) {
}

inline int context::args_count() const {
    return nargs_;
}

inline int context::args_bytes(int idx) const {
    return sqlite3_value_bytes(values_[idx]);
}

inline int context::args_type(int idx) const {
    return sqlite3_value_type(values_[idx]);
}

inline int context::get(int idx, int) const {
    return sqlite3_value_int(values_[idx]);
}

inline double context::get(int idx, double) const {
    return sqlite3_value_double(values_[idx]);
}

inline long long int context::get(int idx, long long int) const {
    return sqlite3_value_int64(values_[idx]);
}

inline char const* context::get(int idx, char const*) const {
    return reinterpret_cast<char const*>(sqlite3_value_text(values_[idx]));
}

inline std::string context::get(int idx, std::string) const {
    return get(idx, (char const*)0);
}

inline void const* context::get(int idx, void const*) const {
    return sqlite3_value_blob(values_[idx]);
}

inline void context::result(int value) {
    sqlite3_result_int(ctx_, value);
}

inline void context::result(double value) {
    sqlite3_result_double(ctx_, value);
}

inline void context::result(long long int value) {
    sqlite3_result_int64(ctx_, value);
}

inline void context::result(std::string const& value) {
    sqlite3_result_text(ctx_, value.c_str(), static_cast<std::int32_t>(value.size()), SQLITE_TRANSIENT);
}

inline void context::result(std::string_view value) {
    sqlite3_result_text(ctx_, value.data(), static_cast<std::int32_t>(value.size()), SQLITE_STATIC);
}

inline void context::result(char const* value, bool fcopy) {
    sqlite3_result_text(ctx_, value, static_cast<std::int32_t>(std::strlen(value)), fcopy ? SQLITE_TRANSIENT : SQLITE_STATIC);
}

inline void context::result(void const* value, int n, bool fcopy) {
    sqlite3_result_blob(ctx_, value, n, fcopy ? SQLITE_TRANSIENT : SQLITE_STATIC);
}

inline void context::result() {
    sqlite3_result_null(ctx_);
}

inline void context::result(null_type) {
    sqlite3_result_null(ctx_);
}

inline void context::result_copy(int idx) {
    sqlite3_result_value(ctx_, values_[idx]);
}

inline void context::result_error(char const* msg) {
    sqlite3_result_error(ctx_, msg, static_cast<std::int32_t>(std::strlen(msg)));
}

inline void* context::aggregate_data(int size) {
    return sqlite3_aggregate_context(ctx_, size);
}

inline int context::aggregate_count() {
    return sqlite3_aggregate_count(ctx_);
}

inline function::function(database& db) : db_(db.db_) {
}

inline int function::create(char const* name, function_handler h, int nargs) {
    fh_[name] = pfunction_base(new function_handler(h));
    return sqlite3_create_function(db_, name, nargs, SQLITE_UTF8, fh_[name].get(), internal::function_impl, 0, 0);
}

inline aggregate::aggregate(database& db) : db_(db.db_) {
}

inline int aggregate::create(char const* name, function_handler s, function_handler f, int nargs) {
    ah_[name] = std::make_pair(pfunction_base(new function_handler(s)), pfunction_base(new function_handler(f)));
    return sqlite3_create_function(db_, name, nargs, SQLITE_UTF8, &ah_[name], 0, internal::step_impl,
                                   internal::finalize_impl);
}
template <class T, class... Ps>
inline int aggregate::create(char const* name) {
    {
        return sqlite3_create_function(db_, name, sizeof...(Ps), SQLITE_UTF8, 0, 0, internal::stepx_impl<T, Ps...>,
                                       internal::finishN_impl<T>);
    }
}
} // namespace ext

} // namespace sqlite
