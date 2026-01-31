#pragma once
#include <system_error>
#ifdef IMPORT_SQLITE_LOADABLE_EXTENSION
    #include <sqlite3ext.h>
#else
    #include <sqlite3.h>
#endif

namespace sqlite
{
namespace error
{
enum sqlite3_errors : std::int32_t
{
    sqlite3_OK         = SQLITE_OK,
    sqlite3_ERROR      = SQLITE_ERROR,
    sqlite3_INTERNAL   = SQLITE_INTERNAL,
    sqlite3_PERM       = SQLITE_PERM,
    sqlite3_ABORT      = SQLITE_ABORT,
    sqlite3_BUSY       = SQLITE_BUSY,
    sqlite3_LOCKED     = SQLITE_LOCKED,
    sqlite3_NOMEM      = SQLITE_NOMEM,
    sqlite3_READONLY   = SQLITE_READONLY,
    sqlite3_INTERRUPT  = SQLITE_INTERRUPT,
    sqlite3_IOERR      = SQLITE_IOERR,
    sqlite3_CORRUPT    = SQLITE_CORRUPT,
    sqlite3_NOTFOUND   = SQLITE_NOTFOUND,
    sqlite3_FULL       = SQLITE_FULL,
    sqlite3_CANTOPEN   = SQLITE_CANTOPEN,
    sqlite3_PROTOCOL   = SQLITE_PROTOCOL,
    sqlite3_EMPTY      = SQLITE_EMPTY,
    sqlite3_SCHEMA     = SQLITE_SCHEMA,
    sqlite3_TOOBIG     = SQLITE_TOOBIG,
    sqlite3_CONSTRAINT = SQLITE_CONSTRAINT,
    sqlite3_MISMATCH   = SQLITE_MISMATCH,
    sqlite3_MISUSE     = SQLITE_MISUSE,
    sqlite3_NOLFS      = SQLITE_NOLFS,
    sqlite3_AUTH       = SQLITE_AUTH,
    sqlite3_FORMAT     = SQLITE_FORMAT,
    sqlite3_RANGE      = SQLITE_RANGE,
    sqlite3_NOTADB     = SQLITE_NOTADB,
    sqlite3_NOTICE     = SQLITE_NOTICE,
    sqlite3_WARNING    = SQLITE_WARNING,
    sqlite3_ROW        = SQLITE_ROW,
    sqlite3_DONE       = SQLITE_DONE,
};

class sqlite_category : public std::error_category {
public:
    static sqlite_category& Instance() {
        static sqlite_category t;
        return t;
    }
    const char* name() const noexcept override {
        return "sqlite3.error";
    }
    std::string message(int value) const override {
        switch (static_cast<sqlite3_errors>(value)) {
        case sqlite3_errors::sqlite3_OK: return "ok";
        case sqlite3_errors::sqlite3_ERROR: return "Generic error";
        case sqlite3_errors::sqlite3_INTERNAL: return "Internal logic error in SQLite";
        case sqlite3_errors::sqlite3_PERM: return "Access permission denied";
        case sqlite3_errors::sqlite3_ABORT: return "Callback routine requested an abort";
        case sqlite3_errors::sqlite3_BUSY: return "The database file is locked";
        case sqlite3_errors::sqlite3_LOCKED: return "A table in the database is locked";
        case sqlite3_errors::sqlite3_NOMEM: return "A malloc() failed";
        case sqlite3_errors::sqlite3_READONLY: return "Attempt to write a readonly database";
        case sqlite3_errors::sqlite3_INTERRUPT: return "Operation terminated by sqlite3_interrupt()";
        case sqlite3_errors::sqlite3_IOERR: return "Some kind of disk I/O error occurred";
        case sqlite3_errors::sqlite3_CORRUPT: return "The database disk image is malformed";
        case sqlite3_errors::sqlite3_NOTFOUND: return "Unknown opcode in sqlite3_file_control()";
        case sqlite3_errors::sqlite3_FULL: return "Insertion failed because database is full";
        case sqlite3_errors::sqlite3_CANTOPEN: return "Unable to open the database file";
        case sqlite3_errors::sqlite3_PROTOCOL: return "Database lock protocol error";
        case sqlite3_errors::sqlite3_EMPTY: return "Internal use only";
        case sqlite3_errors::sqlite3_SCHEMA: return "The database schema changed";
        case sqlite3_errors::sqlite3_TOOBIG: return "String or BLOB exceeds size limit";
        case sqlite3_errors::sqlite3_CONSTRAINT: return "Abort due to constraint violation";
        case sqlite3_errors::sqlite3_MISMATCH: return "Data type mismatch";
        case sqlite3_errors::sqlite3_MISUSE: return "Library used incorrectly";
        case sqlite3_errors::sqlite3_NOLFS: return "Uses OS features not supported on host";
        case sqlite3_errors::sqlite3_AUTH: return "Authorization denied";
        case sqlite3_errors::sqlite3_FORMAT: return "Not used";
        case sqlite3_errors::sqlite3_RANGE: return "2nd parameter to sqlite3_bind out of range";
        case sqlite3_errors::sqlite3_NOTADB: return "File opened that is not a database file";
        case sqlite3_errors::sqlite3_NOTICE: return "Notifications from sqlite3_log()";
        case sqlite3_errors::sqlite3_WARNING: return "Warnings from sqlite3_log()";
        case sqlite3_errors::sqlite3_ROW: return "sqlite3_step() has another row ready";
        case sqlite3_errors::sqlite3_DONE: return "sqlite3_step() has finished executing";
        default: return "sqlite3.error";
        }
    }
};

template <typename T>
inline std::error_code make_error_code(T e) {
    return std::error_code(static_cast<std::int32_t>(e), sqlite_category::Instance());
}
} // namespace error
} // namespace sqlite