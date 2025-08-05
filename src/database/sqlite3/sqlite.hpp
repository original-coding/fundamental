#pragma once
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#include "sqlite-common.hpp"

namespace sqlite
{
class database;
namespace ext
{
class function;
class aggregate;
} // namespace ext

template <class T>
struct convert {
    using to_int = int;
};

class null_type {};

class noncopyable {
protected:
    noncopyable()  = default;
    ~noncopyable() = default;

    noncopyable(noncopyable&&)            = default;
    noncopyable& operator=(noncopyable&&) = default;

    noncopyable(noncopyable const&)            = delete;
    noncopyable& operator=(noncopyable const&) = delete;
};

class database : noncopyable {
    friend class statement;
    friend class database_error;
    friend class ext::function;
    friend class ext::aggregate;

public:
    constexpr static const char* kMemoryDbName = ":memory:";

public:
    using busy_handler      = std::function<int(int)>;
    using commit_handler    = std::function<int()>;
    using rollback_handler  = std::function<void()>;
    using update_handler    = std::function<void(int, char const*, char const*, long long int)>;
    using authorize_handler = std::function<int(int, char const*, char const*, char const*, char const*)>;
    using backup_handler    = std::function<void(int, int, int)>;

    explicit database(char const* dbname,
                      int flags       = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      const char* vfs = nullptr);
    explicit database(int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, const char* vfs = nullptr);
    database(database&& db);
    database& operator=(database&& db);

    static database borrow(sqlite3* pdb);

    ~database();

    int connect(char const* dbname, int flags, const char* vfs = nullptr);
    int disconnect();

    int attach(char const* dbname, char const* name);
    int detach(char const* name);

    int backup(database& destdb, backup_handler h = {});
    int backup(char const* dbname, database& destdb, char const* destdbname, backup_handler h, int step_page = 5);

    long long int last_insert_rowid() const;

    int enable_foreign_keys(bool enable = true);
    int enable_triggers(bool enable = true);
    int enable_extended_result_codes(bool enable = true);

    int changes() const;

    int error_code() const;
    int extended_error_code() const;
    char const* error_msg() const;

    int execute(char const* sql);
    int executef(char const* sql, ...);

    int set_busy_timeout(int ms);

    void set_busy_handler(busy_handler h);
    void set_commit_handler(commit_handler h);
    void set_rollback_handler(rollback_handler h);
    void set_update_handler(update_handler h);
    void set_authorize_handler(authorize_handler h);
    sqlite3* native_handle() {
        return db_;
    }

private:
    database(sqlite3* pdb) : db_(pdb), borrowing_(true) {
    }

private:
    sqlite3* db_;
    bool borrowing_;

    busy_handler bh_;
    commit_handler ch_;
    rollback_handler rh_;
    update_handler uh_;
    authorize_handler ah_;
};

class database_error : public std::runtime_error {
public:
    explicit database_error(char const* msg);
    explicit database_error(database& db);
};

enum copy_semantic
{
    copy,
    nocopy
};

class statement : noncopyable {
public:
    int prepare(char const* stmt);
    int finish();

    int bind(int idx, double value);

    template <typename T, typename = std::enable_if_t<std::is_integral_v<std::decay_t<T>>>>
    int bind(int idx, T value) {
        if constexpr (sizeof(T) <= 4) {
            return sqlite3_bind_int(stmt_, idx, static_cast<std::int32_t>(value));
        } else {
            return sqlite3_bind_int64(stmt_, idx, static_cast<sqlite3_int64>(value));
        }
    }

    int bind(int idx, char const* value, copy_semantic fcopy);
    int bind(int idx, void const* value, std::size_t n, copy_semantic fcopy);
    int bind(int idx, std::string const& value, copy_semantic fcopy);
    int bind(int idx, char16_t const* value, copy_semantic fcopy);
    int bind(int idx);
    int bind(int idx, null_type);

    template <typename T, typename = std::enable_if_t<std::is_integral_v<std::decay_t<T>>>>
    int bind(char const* name, T value) {
        auto idx = sqlite3_bind_parameter_index(stmt_, name);
        return bind(idx, value);
    }

    int bind(char const* name, double value);
    int bind(char const* name, char const* value, copy_semantic fcopy);
    int bind(char const* name, void const* value, std::size_t n, copy_semantic fcopy);
    int bind(char const* name, std::string const& value, copy_semantic fcopy);
    int bind(char const* name);
    int bind(char const* name, null_type);

    int step();
    int reset();
    int clear_bindings();
    sqlite3_stmt* native_handle() {
        return stmt_;
    }

protected:
    explicit statement(database& db, char const* stmt = nullptr);
    ~statement();

    int prepare_impl(char const* stmt);
    int finish_impl(sqlite3_stmt* stmt);

protected:
    database& db_;
    sqlite3_stmt* stmt_;
    char const* tail_;
};

class command : public statement {
public:
    class bindstream {
    public:
        bindstream(command& cmd, int idx);

        template <class T>
        bindstream& operator<<(T value) {
            auto rc = cmd_.bind(idx_, value);
            if (rc != SQLITE_OK) {
                throw database_error(cmd_.db_);
            }
            ++idx_;
            return *this;
        }
        bindstream& operator<<(char const* value) {
            auto rc = cmd_.bind(idx_, value, copy);
            if (rc != SQLITE_OK) {
                throw database_error(cmd_.db_);
            }
            ++idx_;
            return *this;
        }
        bindstream& operator<<(std::string const& value) {
            auto rc = cmd_.bind(idx_, value, copy);
            if (rc != SQLITE_OK) {
                throw database_error(cmd_.db_);
            }
            ++idx_;
            return *this;
        }
        bindstream& operator<<(std::nullptr_t value) {
            auto rc = cmd_.bind(idx_);
            if (rc != SQLITE_OK) {
                throw database_error(cmd_.db_);
            }
            ++idx_;
            return *this;
        }

    private:
        command& cmd_;
        int idx_;
    };

    explicit command(database& db, char const* stmt = nullptr);

    bindstream binder(int idx = 1);

    int execute();
    int execute_all();
};

class query : public statement {
public:
    class rows {
    public:
        class getstream {
        public:
            getstream(rows* rws, int idx);

            template <class T>
            getstream& operator>>(T& value) {
                value = rws_->get<T>(idx_);
                ++idx_;
                return *this;
            }

        private:
            rows* rws_;
            int idx_;
        };

        explicit rows(sqlite3_stmt* stmt);

        int data_count() const;
        int column_type(int idx) const;

        int column_bytes(int idx) const;

        template <class T>
        T get(int idx) const {
            if constexpr (std::is_integral_v<std::decay_t<T>>) {
                if constexpr (sizeof(T) <= 4) {
                    return static_cast<T>(sqlite3_column_int(stmt_, idx));
                } else {
                    return static_cast<T>(sqlite3_column_int64(stmt_, idx));
                }
            } else {
                throw std::runtime_error("imp specialized version");
            }
        }

        template <class... Ts>
        std::tuple<Ts...> get_columns(typename convert<Ts>::to_int... idxs) const {
            return std::make_tuple(get<Ts>(idxs)...);
        }

        getstream getter(int idx = 0);

    private:
        sqlite3_stmt* stmt_;
    };

    class query_iterator {
    public:
        typedef std::input_iterator_tag iterator_category;
        typedef rows value_type;
        typedef std::ptrdiff_t difference_type;
        typedef rows* pointer;
        typedef rows& reference;

        query_iterator();
        explicit query_iterator(query* cmd);

        bool operator==(query_iterator const&) const;
        bool operator!=(query_iterator const&) const;

        query_iterator& operator++();

        value_type operator*() const;

    private:
        query* cmd_;
        int rc_;
    };

    explicit query(database& db, char const* stmt = nullptr);

    int column_count() const;

    char const* column_name(int idx) const;
    char const* column_decltype(int idx) const;

    using iterator = query_iterator;

    iterator begin();
    iterator end();
};

class transaction : noncopyable {
public:
    explicit transaction(database& db, bool fcommit = false, bool freserve = false);
    ~transaction();

    int commit();
    int rollback();

private:
    database* db_;
    bool fcommit_;
};

namespace internal
{
[[maybe_unused]] inline null_type ignore;
inline int busy_handler_impl(void* p, int cnt) {
    auto h = static_cast<database::busy_handler*>(p);
    return (*h)(cnt);
}

inline int commit_hook_impl(void* p) {
    auto h = static_cast<database::commit_handler*>(p);
    return (*h)();
}

inline void rollback_hook_impl(void* p) {
    auto h = static_cast<database::rollback_handler*>(p);
    (*h)();
}

inline void update_hook_impl(void* p, int opcode, char const* dbname, char const* tablename, long long int rowid) {
    auto h = static_cast<database::update_handler*>(p);
    (*h)(opcode, dbname, tablename, rowid);
}

inline int authorizer_impl(void* p,
                           int evcode,
                           char const* p1,
                           char const* p2,
                           char const* dbname,
                           char const* tvname) {
    auto h = static_cast<database::authorize_handler*>(p);
    return (*h)(evcode, p1, p2, dbname, tvname);
}

} // namespace internal

inline database::database(char const* dbname, int flags, char const* vfs) : db_(nullptr), borrowing_(false) {
    if (dbname) {
        auto rc = connect(dbname, flags, vfs);
        if (rc != SQLITE_OK) {
            // Whether or not an error occurs when it is opened, resources
            // associated with the database connection handle should be released
            // by passing it to sqlite3_close() when it is no longer required.
            disconnect();
            throw database_error("can't connect database");
        }
    }
}

inline database::database(int flags, const char* vfs) : database(kMemoryDbName, flags, vfs) {
}

inline database::database(database&& db) :
db_(std::move(db.db_)), borrowing_(std::move(db.borrowing_)), bh_(std::move(db.bh_)), ch_(std::move(db.ch_)),
rh_(std::move(db.rh_)), uh_(std::move(db.uh_)), ah_(std::move(db.ah_)) {
    db.db_ = nullptr;
}

inline database& database::operator=(database&& db) {
    db_        = std::move(db.db_);
    db.db_     = nullptr;
    borrowing_ = std::move(db.borrowing_);

    bh_ = std::move(db.bh_);
    ch_ = std::move(db.ch_);
    rh_ = std::move(db.rh_);
    uh_ = std::move(db.uh_);
    ah_ = std::move(db.ah_);

    return *this;
}

inline database database::borrow(sqlite3* pdb) {
    return database(pdb);
}

inline database::~database() {
    if (!borrowing_) {
        disconnect();
    }
}

inline int database::connect(char const* dbname, int flags, char const* vfs) {
    if (!borrowing_) {
        disconnect();
    }

    return sqlite3_open_v2(dbname, &db_, flags, vfs);
}

inline int database::disconnect() {
    auto rc = SQLITE_OK;
    if (db_) {
        rc = sqlite3_close(db_);

        if (rc == SQLITE_OK) {
            db_ = nullptr;
        }
    }

    return rc;
}

inline int database::attach(char const* dbname, char const* name) {
    return executef("ATTACH '%q' AS '%q'", dbname, name);
}

inline int database::detach(char const* name) {
    return executef("DETACH '%q'", name);
}

inline int database::backup(database& destdb, backup_handler h) {
    return backup("main", destdb, "main", h);
}

inline int database::backup(char const* dbname,
                            database& destdb,
                            char const* destdbname,
                            backup_handler h,
                            int step_page) {
    sqlite3_backup* bkup = sqlite3_backup_init(destdb.db_, destdbname, db_, dbname);
    if (!bkup) {
        return error_code();
    }
    auto rc = SQLITE_OK;
    do {
        rc = sqlite3_backup_step(bkup, step_page);
        if (h) {
            h(sqlite3_backup_remaining(bkup), sqlite3_backup_pagecount(bkup), rc);
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    sqlite3_backup_finish(bkup);
    return rc;
}

inline void database::set_busy_handler(busy_handler h) {
    bh_ = h;
    sqlite3_busy_handler(db_, bh_ ? internal::busy_handler_impl : 0, &bh_);
}

inline void database::set_commit_handler(commit_handler h) {
    ch_ = h;
    sqlite3_commit_hook(db_, ch_ ? internal::commit_hook_impl : 0, &ch_);
}

inline void database::set_rollback_handler(rollback_handler h) {
    rh_ = h;
    sqlite3_rollback_hook(db_, rh_ ? internal::rollback_hook_impl : 0, &rh_);
}

inline void database::set_update_handler(update_handler h) {
    uh_ = h;
    sqlite3_update_hook(db_, uh_ ? internal::update_hook_impl : 0, &uh_);
}

inline void database::set_authorize_handler(authorize_handler h) {
    ah_ = h;
    sqlite3_set_authorizer(db_, ah_ ? internal::authorizer_impl : 0, &ah_);
}

inline long long int database::last_insert_rowid() const {
    return sqlite3_last_insert_rowid(db_);
}

inline int database::enable_foreign_keys(bool enable) {
    return sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_FKEY, enable ? 1 : 0, nullptr);
}

inline int database::enable_triggers(bool enable) {
    return sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_TRIGGER, enable ? 1 : 0, nullptr);
}

inline int database::enable_extended_result_codes(bool enable) {
    return sqlite3_extended_result_codes(db_, enable ? 1 : 0);
}

inline int database::changes() const {
    return sqlite3_changes(db_);
}

inline int database::error_code() const {
    return sqlite3_errcode(db_);
}

inline int database::extended_error_code() const {
    return sqlite3_extended_errcode(db_);
}

inline char const* database::error_msg() const {
    return sqlite3_errmsg(db_);
}

inline int database::execute(char const* sql) {
    return sqlite3_exec(db_, sql, 0, 0, 0);
}

inline int database::executef(char const* sql, ...) {
    va_list ap;
    va_start(ap, sql);
    std::shared_ptr<char> msql(sqlite3_vmprintf(sql, ap), sqlite3_free);
    va_end(ap);

    return execute(msql.get());
}

inline int database::set_busy_timeout(int ms) {
    return sqlite3_busy_timeout(db_, ms);
}

inline statement::statement(database& db, char const* stmt) : db_(db), stmt_(0), tail_(0) {
    if (stmt) {
        auto rc = prepare(stmt);
        if (rc != SQLITE_OK) throw database_error(db_);
    }
}

inline statement::~statement() {
    // finish() can return error. If you want to check the error, call
    // finish() explicitly before this object is destructed.
    finish();
}

inline int statement::prepare(char const* stmt) {
    auto rc = finish();
    if (rc != SQLITE_OK) return rc;

    return prepare_impl(stmt);
}

inline int statement::prepare_impl(char const* stmt) {
    return sqlite3_prepare_v2(db_.db_, stmt, static_cast<std::int32_t>(std::strlen(stmt)), &stmt_, &tail_);
}

inline int statement::finish() {
    auto rc = SQLITE_OK;
    if (stmt_) {
        rc    = finish_impl(stmt_);
        stmt_ = nullptr;
    }
    tail_ = nullptr;

    return rc;
}

inline int statement::finish_impl(sqlite3_stmt* stmt) {
    return sqlite3_finalize(stmt);
}

inline int statement::step() {
    return sqlite3_step(stmt_);
}

inline int statement::reset() {
    return sqlite3_reset(stmt_);
}

inline int statement::clear_bindings() {
    return sqlite3_clear_bindings(stmt_);
}

inline int statement::bind(int idx, double value) {
    return sqlite3_bind_double(stmt_, idx, value);
}

inline int statement::bind(int idx, char const* value, copy_semantic fcopy) {
    return sqlite3_bind_text(stmt_, idx, value, static_cast<std::int32_t>(std::strlen(value)), fcopy == copy ? SQLITE_TRANSIENT : SQLITE_STATIC);
}

inline int statement::bind(int idx, char16_t const* value, copy_semantic fcopy) {
    return sqlite3_bind_text16(stmt_, idx, value, static_cast<std::int32_t>(std::char_traits<char16_t>::length(value) * sizeof(char16_t)),
                               fcopy == copy ? SQLITE_TRANSIENT : SQLITE_STATIC);
}

inline int statement::bind(int idx, void const* value, std::size_t n, copy_semantic fcopy) {
    return sqlite3_bind_blob(stmt_, idx, value, static_cast<std::int32_t>(n), fcopy == copy ? SQLITE_TRANSIENT : SQLITE_STATIC);
}

inline int statement::bind(int idx, std::string const& value, copy_semantic fcopy) {
    return sqlite3_bind_text(stmt_, idx, value.c_str(), static_cast<std::int32_t>(value.size()), fcopy == copy ? SQLITE_TRANSIENT : SQLITE_STATIC);
}

inline int statement::bind(int idx) {
    return sqlite3_bind_null(stmt_, idx);
}

inline int statement::bind(int idx, null_type) {
    return bind(idx);
}

inline int statement::bind(char const* name, double value) {
    auto idx = sqlite3_bind_parameter_index(stmt_, name);
    return bind(idx, value);
}

inline int statement::bind(char const* name, char const* value, copy_semantic fcopy) {
    auto idx = sqlite3_bind_parameter_index(stmt_, name);
    return bind(idx, value, fcopy);
}

inline int statement::bind(char const* name, void const* value, std::size_t n, copy_semantic fcopy) {
    auto idx = sqlite3_bind_parameter_index(stmt_, name);
    return bind(idx, value, static_cast<std::int32_t>(n), fcopy);
}

inline int statement::bind(char const* name, std::string const& value, copy_semantic fcopy) {
    auto idx = sqlite3_bind_parameter_index(stmt_, name);
    return bind(idx, value, fcopy);
}

inline int statement::bind(char const* name) {
    auto idx = sqlite3_bind_parameter_index(stmt_, name);
    return bind(idx);
}

inline int statement::bind(char const* name, null_type) {
    return bind(name);
}

inline command::bindstream::bindstream(command& cmd, int idx) : cmd_(cmd), idx_(idx) {
}

inline command::command(database& db, char const* stmt) : statement(db, stmt) {
}

inline command::bindstream command::binder(int idx) {
    return bindstream(*this, idx);
}

inline int command::execute() {
    auto rc = step();
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

    return rc;
}

inline int command::execute_all() {
    auto rc = execute();
    if (rc != SQLITE_OK) return rc;

    char const* sql = tail_;

    while (std::strlen(sql) > 0) { // sqlite3_complete() is broken.
        sqlite3_stmt* old_stmt = stmt_;

        if ((rc = prepare_impl(sql)) != SQLITE_OK) return rc;

        if ((rc = sqlite3_transfer_bindings(old_stmt, stmt_)) != SQLITE_OK) return rc;

        finish_impl(old_stmt);

        if ((rc = execute()) != SQLITE_OK) return rc;

        sql = tail_;
    }

    return rc;
}

inline query::rows::getstream::getstream(rows* rws, int idx) : rws_(rws), idx_(idx) {
}

inline query::rows::rows(sqlite3_stmt* stmt) : stmt_(stmt) {
}

inline int query::rows::data_count() const {
    return sqlite3_data_count(stmt_);
}

inline int query::rows::column_type(int idx) const {
    return sqlite3_column_type(stmt_, idx);
}

inline int query::rows::column_bytes(int idx) const {
    return sqlite3_column_bytes(stmt_, idx);
}

template <>
inline double query::rows::get<double>(int idx) const {
    return sqlite3_column_double(stmt_, idx);
}

template <>
inline char const* query::rows::get<char const*>(int idx) const {
    return reinterpret_cast<char const*>(sqlite3_column_text(stmt_, idx));
}
template <>
inline char16_t const* query::rows::get<char16_t const*>(int idx) const {
    return reinterpret_cast<char16_t const*>(sqlite3_column_text16(stmt_, idx));
}
template <>
inline std::string query::rows::get<std::string>(int idx) const {
    char const* c = get<const char*>(idx);
    return c ? std::string(c) : std::string();
}
template <>
inline void const* query::rows::get<void const*>(int idx) const {
    return sqlite3_column_blob(stmt_, idx);
}

template <>
inline null_type query::rows::get<null_type>(int /*idx*/) const {
    return {};
}

inline query::rows::getstream query::rows::getter(int idx) {
    return getstream(this, idx);
}

inline query::query_iterator::query_iterator() : cmd_(0) {
    rc_ = SQLITE_DONE;
}

inline query::query_iterator::query_iterator(query* cmd) : cmd_(cmd) {
    rc_ = cmd_->step();
    if (rc_ != SQLITE_ROW && rc_ != SQLITE_DONE) throw database_error(cmd_->db_);
}

inline bool query::query_iterator::operator==(query::query_iterator const& other) const {
    return rc_ == other.rc_;
}

inline bool query::query_iterator::operator!=(query::query_iterator const& other) const {
    return rc_ != other.rc_;
}

inline query::query_iterator& query::query_iterator::operator++() {
    rc_ = cmd_->step();
    if (rc_ != SQLITE_ROW && rc_ != SQLITE_DONE) throw database_error(cmd_->db_);
    return *this;
}

inline query::query_iterator::value_type query::query_iterator::operator*() const {
    return rows(cmd_->stmt_);
}

inline query::query(database& db, char const* stmt) : statement(db, stmt) {
}

inline int query::column_count() const {
    return sqlite3_column_count(stmt_);
}

inline char const* query::column_name(int idx) const {
    return sqlite3_column_name(stmt_, idx);
}

inline char const* query::column_decltype(int idx) const {
    return sqlite3_column_decltype(stmt_, idx);
}

inline query::iterator query::begin() {
    return query_iterator(this);
}

inline query::iterator query::end() {
    return query_iterator();
}

inline transaction::transaction(database& db, bool fcommit, bool freserve) : db_(&db), fcommit_(fcommit) {
    int rc = db_->execute(freserve ? "BEGIN IMMEDIATE" : "BEGIN");
    if (rc != SQLITE_OK) throw database_error(*db_);
}

inline transaction::~transaction() {
    if (db_) {
        // execute() can return error. If you want to check the error,
        // call commit() or rollback() explicitly before this object is
        // destructed.
        db_->execute(fcommit_ ? "COMMIT" : "ROLLBACK");
    }
}

inline int transaction::commit() {
    auto db = db_;
    db_     = nullptr;
    int rc  = db->execute("COMMIT");
    return rc;
}

inline int transaction::rollback() {
    auto db = db_;
    db_     = nullptr;
    int rc  = db->execute("ROLLBACK");
    return rc;
}

inline database_error::database_error(char const* msg) : std::runtime_error(msg) {
}

inline database_error::database_error(database& db) : std::runtime_error(sqlite3_errmsg(db.db_)) {
}
} // namespace sqlite

#ifdef IMPORT_SQLITE_LOADABLE_EXTENSION
inline SQLITE_EXTENSION_INIT1
#endif