#pragma once
#include "fundamental/basic/utils.hpp"
#include "fundamental/events/event_system.h"
#include "object_definitions.h"
#include <string_view>
#include <tuple>

namespace Fundamental
{

/*
///
When using CRTP for multiple inheritance, the final subclass needs to
explicitly specify the method instances to be called for all explicitly
inherited CRTP base class methods, either by using the using directive
to specify the method instances or by reimplementing the methods.

If accessing a CRTP instance is done through the form of base<derived_t>*,
caution must be exercised when releasing the object to ensure that resources
are properly released. In this case, the destructor of derived_t will not be called.
If there are additional resource operations that need to be performed,
 the destructor of the relevant CRTP base class should be declared as a virtual function in such scenarios.
///

template <class derived>
class Base2 {
public:
    using derived_ = derived;

public:
    void Add() {
        imp().Add();
    }
    size_t Result() const {
        return imp().Result();
    }

protected:
    derived& imp() {
        return *(static_cast<derived*>(this));
    };
    const derived& imp() const {
        return *(static_cast<const derived*>(this));
    };
};

class Derived1 : public Base2<Derived1> {
public:
    void Add() {
    }
    size_t Result() const {
        return 0;
    }
};

class Derived2 : public Derived1, public Base2<Derived2> {
public:
    using Derived1::Result;
    void Add() {
        Derived1::Add();
    }
};
*/
using ExpiredSignalType =
    Fundamental::Signal<Fundamental::SignalBrokenType(std::string_view /*table_name*/, std::string_view /*key*/)>;

template <class derived_t>
class data_storage_interface {

public:
    bool init() {
        return derived_t::init();
    }

    std::size_t table_size(std::string table_name) const {
        return derived_t::table_size(std::move(table_name));
    }

    bool has_key(std::string table_name, std::string key) const {
        return derived_t::has_key(std::move(table_name), std::move(key));
    }

    bool remove_data(std::string table_name, std::string key) {
        return derived_t::remove_data(std::move(table_name), std::move(key));
    }

    bool update_key_expired_time(std::string table_name, std::string key, std::int64_t update_expired_time_msec) {
        return derived_t::update_key_expired_time(std::move(table_name), std::move(key), update_expired_time_msec);
    }

    ExpiredSignalType& expired_signal() {
        return derived_t::expired_signal();
    }
};

template <typename value_type, class derived_t>
class data_storage_accessor {
public:
    // return true if persist data success
    bool persist_data(std::string table_name, std::string key, value_type data, const storage_config& config) {
        return derived_t::persist_data(std::move(table_name), std::move(key), std::move(data), config);
    }

    std::tuple<bool, value_type> get_value(std::string table_name, std::string key) const {
        return derived_t::get_value(std::move(table_name), std::move(key));
    }
};

template <class derived_t>
class data_storage_accessor<void, derived_t> {
public:
    using value_type = void;

public:
    bool persist_data(std::string table_name, std::string key, const storage_config& config) {
        return derived_t::persist_data(std::move(table_name), std::move(key), config);
    }
};
} // namespace Fundamental