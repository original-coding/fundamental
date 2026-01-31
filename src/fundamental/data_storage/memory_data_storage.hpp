#pragma once
#include "data_storage_interface.hpp"

#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"

#include <unordered_map>

namespace Fundamental
{

namespace storage
{
struct memory_storage_item_common {
    Fundamental::DelayQueue::HandleType handle;
    std::function<void()> remove_cb = nullptr;
};
template <typename DataType, typename = std::void_t<>>
struct memory_storage_item : memory_storage_item_common {
    DataType data;
};

template <typename DataType>
struct memory_storage_item<DataType, std::void_t<std::enable_if_t<std::is_void_v<DataType>>>>
: memory_storage_item_common {};

template <typename ValueType>
class memory_storage_data : public data_storage_interface<memory_storage_data<ValueType>> {
protected:
    using storage_table_type  = std::unordered_map<std::string, storage::memory_storage_item<ValueType>>;
    using storage_tables_type = std::unordered_map<std::string, storage_table_type>;

public:
    using value_type     = ValueType;
    using iterator       = typename storage_tables_type::iterator;
    using const_iterator = typename storage_tables_type::const_iterator;

public:
    memory_storage_data(DelayQueue* delay_queue) : delay_queue(delay_queue) {
        FASSERT_ACTION(delay_queue != nullptr, throw std::runtime_error("need a valid delay queue ref"),
                       "need a valid delay queu ref");
    }
    ~memory_storage_data() {
        release_data();
    }
    bool init() {
        return true;
    }

    std::size_t table_size(std::string table_name) const {
        std::scoped_lock<std::mutex> locker(data_mutex);
        auto table_iter = storage.find(table_name);
        return table_iter == storage.end() ? 0 : table_iter->second.size();
    }

    bool has_key(std::string table_name, std::string key) const {
        std::scoped_lock<std::mutex> locker(data_mutex);
        auto table_iter = storage.find(table_name);
        return table_iter == storage.end() ? false : table_iter->second.count(key) > 0;
    }

    bool remove_data(std::string table_name, std::string key) {
        std::unique_lock<std::mutex> locker(data_mutex);
        auto table_iter = storage.find(table_name);
        if (table_iter == storage.end()) return false;
        auto data_iter = table_iter->second.find(key);
        if (data_iter == table_iter->second.end()) return false;

        storage::memory_storage_item<ValueType> remove_item = std::move(data_iter->second);
        table_iter->second.erase(data_iter);
        // remove empty table
        if (table_iter->second.empty()) {
            storage.erase(table_iter);
        }
        locker.unlock();
        if (remove_item.handle) delay_queue->RemoveDelayTask(remove_item.handle);
        if (remove_item.remove_cb) remove_item.remove_cb();
        return true;
    }
    bool update_key_expired_time(std::string table_name, std::string key, std::int64_t update_expired_time_msec) {
        std::scoped_lock<std::mutex> locker(data_mutex);
        auto table_iter = storage.find(table_name);
        if (table_iter == storage.end()) return false;
        auto data_iter = table_iter->second.find(key);
        return data_iter == table_iter->second.end()
                   ? false
                   : delay_queue->ModifyTaskNextExpiredTimepoint(data_iter->second.handle, update_expired_time_msec);
    }

    ExpiredSignalType& expired_signal() {
        return expired_signal_;
    }
    // not thread safe
    decltype(auto) begin() {
        return storage.begin();
    }

    decltype(auto) end() {
        return storage.end();
    }

    decltype(auto) cbegin() const {
        return storage.cbegin();
    }
    decltype(auto) cend() const {
        return storage.cend();
    }

    decltype(auto) find(const std::string& table_name) const {
        return storage.find(table_name);
    }

    decltype(auto) find(const std::string& table_name) {
        return storage.find(table_name);
    }

protected:
    void release_data() {
        decltype(storage) copy;
        {
            std::scoped_lock<std::mutex> locker(data_mutex);
            std::swap(copy, storage);
        }

        for (auto& table : copy) {
            for (auto& item : table.second) {
                if (item.second.handle) delay_queue->RemoveDelayTask(item.second.handle);
                if (item.second.remove_cb) item.second.remove_cb();
            }
        }
    }

protected:
    ExpiredSignalType expired_signal_;
    mutable std::mutex data_mutex;
    DelayQueue* const delay_queue;
    storage_tables_type storage;
};

} // namespace storage

template <typename ValueType = std::string>
class memory_storage : public storage::memory_storage_data<ValueType>,
                       public data_storage_accessor<ValueType, memory_storage<ValueType>> {
public:
    using value_type     = ValueType;
    using super          = storage::memory_storage_data<ValueType>;
    using iterator       = typename super::iterator;
    using const_iterator = typename super::const_iterator;

public:
    memory_storage(DelayQueue* delay_queue) : super(delay_queue) {
    }
    ~memory_storage() {
    }
    bool persist_data(std::string table_name, std::string key, value_type data, const storage_config& config) {
        std::scoped_lock<std::mutex> locker(super::data_mutex);
        auto& table                                    = super::storage[table_name];
        auto data_iter                                 = table.find(key);
        storage::memory_storage_item<value_type>& item = data_iter == table.end() ? table[key] : data_iter->second;
        if (data_iter != table.end()) {
            if (!config.overwrite) {
                FWARN("table:{} key:{} has alread existed", table_name, key);
                return false;
            }
            // clear status
            super::delay_queue->RemoveDelayTask(item.handle);
            item.handle.reset();
        }
        item.data      = std::move(data);
        item.remove_cb = config.remove_cb;
        if (config.expired_time_msec > 0) {
            item.handle = super::delay_queue->AddDelayTask(
                config.expired_time_msec,
                [table_name, key, this]() {
                    super::remove_data(table_name, key);
                    super::expired_signal_.Emit(table_name, key);
                },
                true, false);
            super::delay_queue->StartDelayTask(item.handle);
        }
        return true;
    }

    std::tuple<bool, value_type> get_value(std::string table_name, std::string key) const {
        bool b_sucess = false;
        value_type ret;

        do {
            std::scoped_lock<std::mutex> locker(super::data_mutex);
            auto table_iter = super::storage.find(table_name);
            if (table_iter == super::storage.end()) break;
            auto data_iter = table_iter->second.find(key);
            if (data_iter == table_iter->second.end()) break;
            ret      = data_iter->second.data;
            b_sucess = true;
        } while (0);
        return { b_sucess, std::move(ret) };
    }
};

template <>
class memory_storage<void> : public storage::memory_storage_data<void>,
                             public data_storage_accessor<void, memory_storage<void>> {
public:
    using value_type     = void;
    using super          = storage::memory_storage_data<void>;
    using iterator       = typename super::iterator;
    using const_iterator = typename super::const_iterator;

public:
    memory_storage(DelayQueue* delay_queue) : super(delay_queue) {
    }
    ~memory_storage() {
    }
    bool persist_data(std::string table_name, std::string key, const storage_config& config) {
        std::scoped_lock<std::mutex> locker(super::data_mutex);
        auto& table                                    = super::storage[table_name];
        auto data_iter                                 = table.find(key);
        storage::memory_storage_item<value_type>& item = data_iter == table.end() ? table[key] : data_iter->second;
        if (data_iter != table.end()) {
            if (!config.overwrite) {
                FWARN("table:{} key:{} has alread existed", table_name, key);
                return false;
            }
            // clear status
            super::delay_queue->RemoveDelayTask(item.handle);
            item.handle.reset();
        }
        item.remove_cb = config.remove_cb;
        if (config.expired_time_msec > 0) {
            item.handle = super::delay_queue->AddDelayTask(
                config.expired_time_msec,
                [table_name, key, this]() {
                    super::remove_data(table_name, key);
                    expired_signal_.Emit(table_name, key);
                },
                true, false);
            super::delay_queue->StartDelayTask(item.handle);
        }
        return true;
    }
};
} // namespace Fundamental