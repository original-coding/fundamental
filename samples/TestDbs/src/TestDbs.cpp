
#include "fundamental/basic/log.h"

#include <chrono>
#include "fundamental/basic/cxx_config_include.hpp"
#include <iostream>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#define FORCE_TIME_TRACKER 1

#include "database/sqlite3/sqlite.hpp"
#include "fundamental/basic/parallel.hpp"
#include "fundamental/basic/utils.hpp"
#include "fundamental/tracker/time_tracker.hpp"

static constexpr std::size_t kGroupSize     = 400 * 1024; // 400k
static constexpr std::size_t kHashGroupSize = 65536;
void test_sqlite3(std::size_t test_nums);
void test_rocksdb(std::size_t test_nums);
int main(int argc, char* argv[]) {
    std::size_t test_nums = 100000;
    if (argc > 1) {
        try {
            test_nums = std::stoull(argv[1]);
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
        }
    }
    FINFO("using test_nums:{}", test_nums);
    test_sqlite3(test_nums);
    test_rocksdb(test_nums);
    return 0;
}

void test_sqlite3(std::size_t test_nums) {
    using Type   = Fundamental::STimeTracker<std::chrono::milliseconds>;
    auto db_name = "test.sqlite3.db";
    try {
        std_fs::remove(db_name);
    } catch (const std::exception& e) {
    }
    DeclareTimeTacker(Type, t, "test sqlite3 all", Fundamental::StringFormat("nums:{}", test_nums), 10, true, nullptr);
    sqlite::database db(db_name);
    db.execute(R"""(
                    CREATE TABLE IF NOT EXISTS pir (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    h1 INTEGER NOT NULL,
                    h2 INTEGER NOT NULL,
                    g INTEGER NOT NULL,
                    i INTEGER NOT NULL,
                    UNIQUE(h1,h2)
                 );
         )""");
    std::mutex mutex;
    auto hash_group_size = kHashGroupSize;
    std::vector<std::int64_t> hash_indexes_cache(hash_group_size, -1);
    {
        DeclareTimeTacker(Type, t, "test sqlite3 build", Fundamental::StringFormat("nums:{}", test_nums), 10, true,
                          nullptr);
        auto handle_func = [&](std::size_t start_index, std::size_t nums, std::size_t /*group*/
                           ) {
            std::scoped_lock<std::mutex> locker(mutex);
            sqlite::transaction t(db);
            while (nums > 0) {
                sqlite::command cmd(db, "INSERT INTO pir (h1, h2,g,i) VALUES (?,?,?,?)");
                cmd.bind(1, start_index);
                cmd.bind(2, start_index);
                std::uint64_t index = start_index;
                std::int64_t group  = start_index;
                cmd.bind(3, group);
                cmd.bind(4, index);
                cmd.execute();
                --nums;
                ++start_index;
            }
            t.commit();
        };
        // process query gen
        Fundamental::ParallelRun((std::size_t)0, test_nums, handle_func, kGroupSize);
    }
    hash_group_size *= 2;
    hash_indexes_cache = std::vector<std::int64_t>(hash_group_size, -1);
    {
        DeclareTimeTacker(Type, t, "test sqlite3 update size", Fundamental::StringFormat("nums:{}", test_nums), 10,
                          true, nullptr);
        std::size_t max_id = 0;
        {
            sqlite::query q(db, "SELECT MAX(id) FROM pir;");
            auto iter = q.begin();
            if (iter != q.end()) {
                auto row = *iter;
                max_id   = row.get<std::size_t>(0);
            }
        }
        FASSERT_ACTION(max_id == test_nums, , "sqlite3 miss items need:{} actual:{}", test_nums, max_id);
        max_id += 1;

        for (std::size_t i = 1; i < max_id;) {
            auto update_start = i;
            i                 = i + kGroupSize;
            if (i > max_id) i = max_id;

            sqlite::query qry(db, "SELECT id,h1,g,i FROM pir WHERE id >= ? AND id < ?");
            qry.bind(1, update_start);
            qry.bind(2, i);
            auto iter = qry.begin();
            if (iter == qry.end()) {
                continue;
            }
            std::scoped_lock<std::mutex> locker(mutex);
            sqlite::transaction t(db);
            [[maybe_unused]] std::size_t count = 0;
            while (iter != qry.end()) {
                ++count;
                auto row                    = *iter;
                std::uint64_t id            = row.get<std::uint64_t>(0);
                std::uint64_t h1            = row.get<std::uint64_t>(1);
                [[maybe_unused]] auto old_g = row.get<std::uint64_t>(2);
                [[maybe_unused]] auto old_i = row.get<std::uint64_t>(3);
                FASSERT_ACTION(h1 == old_g && h1 == old_i, , "{}  insert failed for sqlite3", h1);
                std::uint64_t index = h1 % hash_group_size;
                ++hash_indexes_cache[index];
                std::int64_t group = hash_indexes_cache[index];
                sqlite::command cmd(db, "UPDATE pir SET g = ?, i = ? WHERE id = ?;");
                cmd.bind(1, group);
                cmd.bind(2, index);
                cmd.bind(3, id);
                cmd.execute();
                ++iter;
            }
            t.commit();
        }
    }
    {
        DeclareTimeTacker(Type, t, "test sqlite3 load cache", Fundamental::StringFormat("nums:{}", test_nums), 10, true,
                          nullptr);
        auto tmp_cache = std::vector<std::int64_t>(hash_group_size, -1);
        do {
            sqlite::query qry(db, "SELECT i,MAX(g) FROM pir GROUP BY i");
            auto iter = qry.begin();
            while (iter != qry.end()) {
                auto row     = *iter;
                auto i       = row.get<std::uint64_t>(0);
                auto g       = row.get<std::int64_t>(1);
                tmp_cache[i] = g;
                ++iter;
            }
        } while (0);
        StopTimeTracker(t);
        if (tmp_cache == hash_indexes_cache) {
            FINFO("sqlite3 verify success");
        } else {
            FERR("sqlite3 verify failed, {} {}", tmp_cache.size(), hash_indexes_cache.size());
            for (std::size_t i = 0; i < tmp_cache.size(); ++i) {
                auto g1 = tmp_cache[i];
                auto g2 = hash_indexes_cache[i];
                FASSERT_ACTION(g1 == g2, continue, "sqlite3 index:{} not {} equal {}", i, g1, g2);
            }
        }
    }
}

template <typename Value1, typename Value2, typename = std::enable_if_t<sizeof(Value1) == 8 && sizeof(Value2) == 8>>
struct rocksdb_data_item {
    using first_t                          = Value1;
    using second_t                         = Value2;
    static constexpr std::size_t kDataSize = sizeof(first_t) + sizeof(second_t);
    rocksdb_data_item()                    = default;
    explicit rocksdb_data_item(const rocksdb::Slice& slice) {
        if (slice.size() >= kDataSize) {
            std::memcpy(data(), slice.data(), kDataSize);
        }
    }
    template <typename U1, typename U2>
    explicit rocksdb_data_item(U1 first, U2 second) : first(first), second(second) {
    }
    inline const char* data() const {
        return reinterpret_cast<const char*>(this);
    }
    inline char* data() {
        return reinterpret_cast<char*>(this);
    }
    inline std::size_t size() const {
        return kDataSize;
    }
    inline rocksdb::Slice to_slice() const {
        return rocksdb::Slice(data(), size());
    }
    first_t first;
    second_t second;
};

template <typename T, typename = std::enable_if_t<sizeof(T) == 8>>
struct rocksdb_cache_item_helper {
    using value_type            = T;
    rocksdb_cache_item_helper() = default;
    template <typename U>
    explicit rocksdb_cache_item_helper(U value) : value(value) {
    }
    explicit rocksdb_cache_item_helper(const rocksdb::Slice& slice) {
        if (slice.size() >= kDataSize) {
            std::memcpy(&value, slice.data(), kDataSize);
        }
    }
    static constexpr std::size_t kDataSize = sizeof(value_type);
    inline const char* data() const {
        return reinterpret_cast<const char*>(&value);
    }
    inline char* data() {
        return reinterpret_cast<char*>(&value);
    }
    inline std::size_t size() const {
        return kDataSize;
    }
    inline rocksdb::Slice to_slice() const {
        return rocksdb::Slice(data(), size());
    }
    value_type value;
};

void test_rocksdb(std::size_t test_nums) {
    using Type   = Fundamental::STimeTracker<std::chrono::milliseconds>;
    auto db_name = "test.rocksdb.db";
    try {
        std_fs::remove_all(db_name);
    } catch (const std::exception& e) {
    }
    DeclareTimeTacker(Type, t, "test rocksdb all", Fundamental::StringFormat("nums:{}", test_nums), 10, true, nullptr);
    rocksdb::DB* db = nullptr;

    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    column_families.push_back(
        rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));
    column_families.push_back(rocksdb::ColumnFamilyDescriptor("index_cache", rocksdb::ColumnFamilyOptions()));

    std::vector<rocksdb::ColumnFamilyHandle*> handles;

    rocksdb::Options options;
    options.create_if_missing              = true;
    options.create_missing_column_families = true;
    options.IncreaseParallelism(4);
    options.OptimizeLevelStyleCompaction();
    options.write_buffer_size                = 64 * 1024 * 1024; // 64MB
    options.max_write_buffer_number          = 4;
    options.min_write_buffer_number_to_merge = 2;

    rocksdb::Status status = rocksdb::DB::Open(options, db_name, column_families, &handles, &db);

    FASSERT_ACTION(status.ok() && handles.size() == 2, return, " can open rocksdb for error {} column_size:{}",
                   status.ToString(), handles.size());
    Fundamental::ScopeGuard g([&]() {
        for (auto& handle : handles) {
            db->DestroyColumnFamilyHandle(handle);
        }
        db->Close();
        delete db;
    });
    auto* default_handle     = handles[0];
    auto* cache_handle       = handles[1];
    auto write_options       = rocksdb::WriteOptions();
    write_options.sync       = false;
    write_options.disableWAL = true;

    auto read_options             = rocksdb::ReadOptions();
    read_options.fill_cache       = false;
    read_options.verify_checksums = false;

    auto hash_group_size = kHashGroupSize;
    std::vector<std::int64_t> hash_indexes_cache(hash_group_size, -1);
    using data_key   = rocksdb_data_item<std::size_t, std::size_t>;
    using data_value = rocksdb_data_item<std::int64_t, std::size_t>;
    {
        DeclareTimeTacker(Type, t, "test rocksdb build", Fundamental::StringFormat("nums:{}", test_nums), 10, true,
                          nullptr);
        auto handle_func = [&](std::size_t start_index, std::size_t nums, std::size_t group) {
            rocksdb::WriteBatch batch_writer;
            std::vector<std::pair<data_key, data_value>> storage;
            while (nums > 0) {
                auto& new_item         = storage.emplace_back();
                new_item.first.first   = start_index;
                new_item.first.second  = start_index;
                new_item.second.first  = start_index;
                new_item.second.second = start_index;
                batch_writer.Put(default_handle, new_item.first.to_slice(), new_item.second.to_slice());
                --nums;
                ++start_index;
            }
            auto commit_status = db->Write(write_options, &batch_writer);
            FASSERT_ACTION(commit_status.ok(), return, "rocksdb commit failed for group {} {}", group,
                           commit_status.ToString());
        };
        // process query gen
        Fundamental::ParallelRun((std::size_t)0, test_nums, handle_func, kGroupSize);
        rocksdb::WriteBatch batch_writer;
        for (std::uint64_t index = 0; index < test_nums; ++index) {
            rocksdb_cache_item_helper<std::uint64_t> key(index);
            rocksdb_cache_item_helper<std::int64_t> value(index);
            batch_writer.Put(cache_handle, key.to_slice(), value.to_slice());
        }
        auto commit_status = db->Write(write_options, &batch_writer);
        FASSERT_ACTION(commit_status.ok(), return, "rocksdb commit failed for index cache {}",
                       commit_status.ToString());
    }
    hash_group_size *= 2;
    hash_indexes_cache = std::vector<std::int64_t>(hash_group_size, -1);
    do {
        DeclareTimeTacker(Type, t, "test rocksdb update size", Fundamental::StringFormat("nums:{}", test_nums), 10,
                          true, nullptr);
        std::size_t data_nums = 0;

        rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions(), default_handle);
        if (!it) break;
        Fundamental::ScopeGuard g([&]() { delete it; });

        {
            std::shared_ptr<rocksdb::WriteBatch> batch = std::make_shared<rocksdb::WriteBatch>();
            Fundamental::ScopeGuard flush_g([&]() {
                if (batch->Count() > 0) {
                    db->Write(write_options, batch.get());
                }
            });
            Fundamental::ThreadPoolConfig config;
            config.min_work_threads_num = 0;
            config.max_threads_limit    = 8;

            Fundamental::ThreadPool pool;
            pool.InitThreadPool(config);

            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                ++data_nums;
                data_key key(it->key());
                data_value value(it->value());

                FASSERT_ACTION(key.first == value.second && key.first == static_cast<data_key::first_t>(value.first),
                               return, "{} {} {} insert failed for rocksdb", key.first, value.second, value.first);
                value.second = key.first % hash_group_size;
                ++hash_indexes_cache[value.second];
                value.first = hash_indexes_cache[value.second];
                batch->Put(default_handle, key.to_slice(), value.to_slice());
                if (batch->Count() >= kGroupSize) {
                    pool.Enqueue([db, batch = std::move(batch), write_options]() mutable {
                        db->Write(write_options, batch.get());
                    });
                    batch = std::make_shared<rocksdb::WriteBatch>();
                }
            }
            pool.WaitAllTaskFinished();
        }
        rocksdb::WriteBatch batch_writer;
        for (std::uint64_t index = 0; index < hash_group_size; ++index) {
            rocksdb_cache_item_helper<std::uint64_t> key(index);
            rocksdb_cache_item_helper<std::int64_t> value(hash_indexes_cache[index]);
            batch_writer.Put(cache_handle, key.to_slice(), value.to_slice());
        }
         auto commit_status = db->Write(write_options, &batch_writer);
        FASSERT_ACTION(commit_status.ok(), return, "rocksdb commit failed for index cache {}",
                       commit_status.ToString());
        FASSERT_ACTION(data_nums == test_nums, , "rocksdb miss nums need:{} actual:{}", test_nums, data_nums);
    } while (0);
    {
        DeclareTimeTacker(Type, t, "test rocksdb load cache", Fundamental::StringFormat("nums:{}", test_nums), 10, true,
                          nullptr);
        auto tmp_cache = std::vector<std::int64_t>(hash_group_size, -1);
        do {
            rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions(), cache_handle);
            if (!it) break;
            Fundamental::ScopeGuard g([&]() { delete it; });

            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                rocksdb_cache_item_helper<std::uint64_t> key(it->key());
                rocksdb_cache_item_helper<std::int64_t> value(it->value());
                // maybe hash _group size scale with a param < 1
                if (key.value < hash_group_size) {
                    tmp_cache[key.value] = value.value;
                }
            }

        } while (0);
        StopTimeTracker(t);
        if (tmp_cache == hash_indexes_cache) {
            FINFO("rocksdb verify success");
        } else {
            FERR("rocksdb verify failed, {} {}", tmp_cache.size(), hash_indexes_cache.size());
            for (std::size_t i = 0; i < tmp_cache.size(); ++i) {
                auto g1 = tmp_cache[i];
                auto g2 = hash_indexes_cache[i];
                FASSERT_ACTION(g1 == g2, continue, "rocksdb index:{} not {} equal {}", i, g1, g2);
            }
        }
    }
}
