#pragma once
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4996 26498 26495 26819 26800)
#endif
#include "spdlog/spdlog.h"
#include <spdlog/fmt/ostr.h> //suport fmt custom types with  operator<<
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

#if TARGET_PLATFORM_WINDOWS
    #include <string>
#endif

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

#include "error_code.hpp"

namespace Fundamental
{

template <typename _CharT, typename _Traits>
inline std::basic_ostream<_CharT, _Traits>& operator<<(std::basic_ostream<_CharT, _Traits>& __os,
                                                       const Fundamental::error_code& code) {
    return (__os << code.category().name() << ":" << code.value() << "(" << code.message() << ") "
                 << code.details_view());
}

enum LogLevel
{
    trace    = 0,
    debug    = 1,
    info     = 2,
    warn     = 3,
    err      = 4,
    critical = 5,
    off      = 6
};
using ErrorHandlerType    = std::function<void(const std::string&)>;
using LogMessageCatchFunc = std::function<void(LogLevel level, const char* /*str*/, std::size_t /*size*/)>;
namespace details
{
class NativeLogSink;
}
class Logger {
    friend class details::NativeLogSink;
    template <typename Arg1, typename... Args>
    friend std::string StringFormat(const char* fmt, const Arg1& arg1, const Args&... args);

public:
    struct LoggerInitOptions {
        static constexpr LogLevel kDefaultLogLevel                = LogLevel::trace;
        static constexpr const char* kDefaultCustomLoggerName     = "logger";
        static constexpr const char* kDefaultLogOutputPath        = "logs";
        static constexpr const char* kDefaultLogOutputProgramName = "";
        static constexpr const char* kDefaultLogOutputFileExt     = ".log";
        static constexpr const char* kDefaultLogFormat            = "%^%Y-%m-%d %H:%M:%S.%e %l%$ [thread:%t]: %v";
        static constexpr std::int32_t kDefaultRotationHour        = 23;
        static constexpr std::int32_t kDefaultRotationMin         = 0;
        static constexpr std::size_t kDefaultLogFileLimitSize     = 10 * 1024 * 1024; // 10M
        // 2 month
        static constexpr std::int64_t kDefaultMaxLogReserveTimeSec =
            60 /*min*/ * 60 /*hour*/ * 24 /*day*/ * 30 /*mon*/ * 2;
        std::string loggerName = kDefaultCustomLoggerName;
        // minimum log level
        std::optional<LogLevel> minimumLevel;
        // log output path
        std::optional<std::string> logOutputPath;
        // log output filebasename
        std::optional<std::string> logOutputProgramName;
        // log file's ext
        std::optional<std::string> logOutputFileExt;
        // control logs' head format
        std::optional<std::string> logFormat;

        // eg: if the rotationHour=17 rotationMin=0
        // when the day's time come to 17:00
        // new file destination will be used
        //  log file's rotation hour in a day
        std::optional<std::int32_t> rotationHour;
        // log file's rotation minute in a day
        std::optional<std::int32_t> rotationMin;
        // single log file's limit size
        std::optional<std::size_t> logFileLimitSize;
        // llean up log files that have existed for more than maxLogReserveTimeSec
        std::optional<std::int64_t> maxLogReserveTimeSec;
        // control log to console
        bool enableConsoleOutput = true;
        //
        ErrorHandlerType errorHandler = nullptr;
        // catch all log msg
        LogMessageCatchFunc catchHandler = nullptr;
    };

public:
    Logger(const std::string_view& format_str = "%^[%L]%$ %v");
    ~Logger();

    template <typename T>
    inline void LogOutput(LogLevel level, const T& data) {
        if (level < LogLevel::warn) {
            loggerStorage->log(static_cast<spdlog::level::level_enum>(level), data);
        } else {
            loggerStderrStorage->log(static_cast<spdlog::level::level_enum>(level), data);
        }
    }

    template <typename Arg1, typename... Args>
    inline void LogOutput(LogLevel level, const char* fmt, const Arg1& arg1, const Args&... args) {
        if (level < LogLevel::warn) {
            loggerStorage->log(static_cast<spdlog::level::level_enum>(level), fmt, arg1,
                               std::forward<const Args&>(args)...);
        } else {
            loggerStderrStorage->log(static_cast<spdlog::level::level_enum>(level), fmt, arg1,
                                     std::forward<const Args&>(args)...);
        }
    }
    static void Initialize(LoggerInitOptions options, Logger* logger = s_defaultLogger);
    static void Release(Logger* logger = s_defaultLogger);
    static bool IsDebuggerAttached();
    static void PrintBackTrace();
    static void TestLogInstance();

public:
    static Logger* s_defaultLogger;

private:
    ErrorHandlerType errorHandler;
    // log output
    std::shared_ptr<details::NativeLogSink> nativeLogSink = nullptr;
    std::shared_ptr<spdlog::logger> loggerStorage         = nullptr;
    std::shared_ptr<spdlog::logger> loggerStderrStorage   = nullptr;
};

inline std::string StringFormat() {
    return "";
}

template <typename T>
inline std::string StringFormat(const T& data) {
    std::stringstream ss_;
    ss_ << data;
    return ss_.str();
}

inline std::string StringFormat(const char* data) {
    return std::string(data, strlen(data));
}

template <typename Arg1, typename... Args>
inline std::string StringFormat(const char* fmt, const Arg1& arg1, const Args&... args) {
    try {
        spdlog::details::log_msg msg;
        msg.raw.write(fmt, arg1, args...);
        static spdlog::pattern_formatter* s_string_formatter =
            new spdlog::pattern_formatter("%v", spdlog::pattern_time_type::local, "");
        s_string_formatter->format(msg);
        return std::string(msg.formatted.data(), msg.formatted.size());
    } catch (const std::exception& e) {
        if (Logger::s_defaultLogger->errorHandler) Logger::s_defaultLogger->errorHandler(e.what());
        return "";
    }
}

inline std::string to_string(const Fundamental::error_code& code) {
    return StringFormat(code);
}

class LoggerStream final {
public:
    explicit LoggerStream(Logger* logger, LogLevel level);
    explicit LoggerStream(Logger* logger,
                          LogLevel level,
                          const char* fileName,
                          const char* funcName,
                          std::int32_t line);
    explicit LoggerStream(Logger* logger, LogLevel level, std::int32_t line);
    ~LoggerStream();
    std::stringstream& stream() {
        return ss_;
    }

    template <typename T>
    LoggerStream& operator<<(const T&) {
        // do nothing
        return *this;
    }

    template <typename T>
    LoggerStream& operator>>(T&) {
        // do nothing
        return *this;
    }

    LoggerStream& operator<<(std::ostream& (*)(std::ostream&)) {
        // do nothing
        return *this;
    }
    LoggerStream& null_stream() {
        enable_output = false;
        return *this;
    }

private:
    Logger* const loggerRef = nullptr;
    const LogLevel level_;
    std::stringstream ss_;
    bool enable_output = true;
};
} // namespace Fundamental

template <std::size_t level, char SlashChar, bool with_discard_flag, std::size_t N>
inline constexpr std::array<char, N> __get_short_file_name__(const char (&file_name)[N]) {
    static_assert(level > 0);
    std::array<char, N> short_name {};
    if constexpr (N <= level) {
#if TARGET_PLATFORM_WINDOWS
        std::memcpy(short_name.data(), file_name, N);
#else
        __builtin_memcpy(short_name.data(), file_name, N);
#endif
        return short_name;
    }
    std::size_t find_level = 0;
    std::size_t i          = N - 1;
    for (; i != 0 && find_level < level; --i) {
        if (file_name[i] == SlashChar) {
            ++find_level;
        }
    }
    if (find_level < level) {
#if TARGET_PLATFORM_WINDOWS
        std::memcpy(short_name.data(), file_name, N);
#else
        __builtin_memcpy(short_name.data(), file_name, N);
#endif
        return short_name;
    }
    std::size_t flag_bytes = with_discard_flag ? i : 0;
    if (flag_bytes > 3) flag_bytes = 3;
    std::size_t copy_size = std::min(N - i - 1, N - flag_bytes - 1); // -1 for null terminator
#if TARGET_PLATFORM_WINDOWS
    std::memset(short_name.data(), '.', flag_bytes);
#else
    __builtin_memset(short_name.data(), '.', flag_bytes);
#endif

#if TARGET_PLATFORM_WINDOWS
    std::memcpy(short_name.data() + flag_bytes, file_name + i + 1, copy_size);
#else
    __builtin_memcpy(short_name.data() + flag_bytes, file_name + i + 1, copy_size);
#endif
    short_name[flag_bytes + copy_size] = '\0';
    return short_name;
}
#define SHORT_FILE_NAME_LEVEL        3
#define SHORT_FILE_NAME_SLASH_CHAR   '/'
#define SHORT_FILE_NAME_DISCARD_FLAG true
#define SHORT_FILE_NAME                                                                                                \
    __get_short_file_name__<SHORT_FILE_NAME_LEVEL, SHORT_FILE_NAME_SLASH_CHAR, SHORT_FILE_NAME_DISCARD_FLAG>(__FILE__) \
        .data()

#ifdef NO_SHORT_FILE_NAME
    #define LOG_FILE_NAME __FILE__
#else
    #if defined(__LOG_PRINT_FILE_NAME__)
        #define LOG_FILE_NAME SHORT_FILE_NAME
    #else
        #define LOG_FILE_NAME ""
    #endif
#endif

#define STR_H(x)      #x
#define STR_HELPER(x) STR_H(x)

#ifndef DISABLE_FLOG
    #define FLOG(logger, logLevel, ...) (logger)->LogOutput(logLevel, __VA_ARGS__)
    #if defined(__LOG_PRINT_FILE_NAME__)
        #define FLOG_DEBUGINFO(logger, logLevel, ...)                                                                  \
            do {                                                                                                       \
                auto __debugInfo__ = Fundamental::StringFormat("[{}:{}"                                                \
                                                               "(" STR_HELPER(__LINE__) ")] ",                         \
                                                               LOG_FILE_NAME, __func__);                               \
                (logger)->LogOutput(logLevel, __debugInfo__ + Fundamental::StringFormat(__VA_ARGS__));                 \
            } while (0)
    #else
        #define FLOG_DEBUGINFO(logger, logLevel, ...)                                                                  \
            do {                                                                                                       \
                auto __debugInfo__ = Fundamental::StringFormat("[{}"                                                   \
                                                               "(" STR_HELPER(__LINE__) ")] ",                         \
                                                               __func__);                                              \
                (logger)->LogOutput(logLevel, __debugInfo__ + Fundamental::StringFormat(__VA_ARGS__));                 \
            } while (0)
    #endif
#else
    #define FLOG(logger, logLevel, ...)           (void)0
    #define FLOG_DEBUGINFO(logger, logLevel, ...) (void)0
#endif // !DISABLE_FLOG

#ifdef DEBUG
    #define FDEBUG(...)           FLOG(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::debug, __VA_ARGS__)
    #define FDEBUG_I(logger, ...) FLOG(logger, Fundamental::LogLevel::debug, __VA_ARGS__)
#else
    #define FDEBUG(...)           (void)0
    #define FDEBUG_I(logger, ...) (void)0
#endif

#ifndef DISABLE_TRACE
    #define FTRACE(...)                                                                                                \
        FLOG_DEBUGINFO(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::trace, ##__VA_ARGS__)
#else
    #define FTRACE(...) (void)0
#endif

#define FINFO(...) FLOG(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::info, ##__VA_ARGS__)
#define FERR(...)  FLOG_DEBUGINFO(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::err, ##__VA_ARGS__)
#define FFAIL(...) FLOG_DEBUGINFO(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::critical, ##__VA_ARGS__)
#define FWARN(...) FLOG_DEBUGINFO(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::warn, ##__VA_ARGS__)

#ifdef DEBUG
    #define FDEBUGS                                                                                                    \
        Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::debug).stream()
#else
    #define FDEBUGS                                                                                                    \
        Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::debug).null_stream()
#endif

#define FINFOS Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::info).stream()
#define FINFOSL                                                                                                        \
    Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::info, __LINE__).stream()
#define FERRS                                                                                                          \
    Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::err, LOG_FILE_NAME,         \
                              __func__, __LINE__)                                                                      \
        .stream()
#define FFAILS                                                                                                         \
    Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::critical, LOG_FILE_NAME,    \
                              __func__, __LINE__)                                                                      \
        .stream()
#define FWARNS                                                                                                         \
    Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::warn, LOG_FILE_NAME,        \
                              __func__, __LINE__)                                                                      \
        .stream()
#ifndef DISABLE_TRACE
    #define FTRACES                                                                                                    \
        Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::trace, LOG_FILE_NAME,   \
                                  __func__, __LINE__)                                                                  \
            .stream()
#else
    #define FTRACES                                                                                                    \
        Fundamental::LoggerStream(Fundamental::Logger::s_defaultLogger, Fundamental::LogLevel::trace, LOG_FILE_NAME,   \
                                  __func__, __LINE__)                                                                  \
            .null_stream()
#endif
#if defined(DEBUG) && !defined(DISABLE_ASSERT)
    #define ENABLE_FUNDAMENTAL_ASSERT_MACRO 1
    #if defined(__LOG_PRINT_FILE_NAME__)
        #define FASSERT_THROWN(_check, ...)                                                                            \
            if (!(_check)) {                                                                                           \
                auto __debugInfo__ =                                                                                   \
                    Fundamental::StringFormat("[{}:{}"                                                                 \
                                              "(" STR_HELPER(__LINE__) ")] [####check####:" #_check "] ",              \
                                              LOG_FILE_NAME, __func__);                                                \
                auto __debug_msg__ = __debugInfo__ + Fundamental::StringFormat(__VA_ARGS__);                           \
                Fundamental::Logger::s_defaultLogger->LogOutput(Fundamental::LogLevel::critical, __debug_msg__);       \
                throw std::runtime_error(__debug_msg__);                                                               \
            }
    #else
        #define FASSERT_THROWN(_check, ...)                                                                            \
            if (!(_check)) {                                                                                           \
                auto __debugInfo__ =                                                                                   \
                    Fundamental::StringFormat("[{}"                                                                    \
                                              "(" STR_HELPER(__LINE__) ")] [####check####:" #_check "] ",              \
                                              __func__);                                                               \
                auto __debug_msg__ = __debugInfo__ + Fundamental::StringFormat(__VA_ARGS__);                           \
                Fundamental::Logger::s_defaultLogger->LogOutput(Fundamental::LogLevel::critical, __debug_msg__);       \
                throw std::runtime_error(__debug_msg__);                                                               \
            }
    #endif
#else
    #define FASSERT_THROWN(_check, ...)                                                                                \
        if constexpr (false) {                                                                                         \
            ((void)(0 && (_check) && Fundamental::StringFormat(__VA_ARGS__).empty()));                                 \
        }
#endif

#define FASSERT(_check, ...) FASSERT_THROWN(_check, ##__VA_ARGS__)
#if defined(__LOG_PRINT_FILE_NAME__)
    #define FASSERT_ACTION(_check, _action, ...)                                                                       \
        if (!(_check)) {                                                                                               \
            auto __debugInfo__ = Fundamental::StringFormat("[{}:{}"                                                    \
                                                           "(" STR_HELPER(__LINE__) ")] [####check####:" #_check "] ", \
                                                           LOG_FILE_NAME, __func__);                                   \
            Fundamental::Logger::s_defaultLogger->LogOutput(Fundamental::LogLevel::warn,                               \
                                                            __debugInfo__ + Fundamental::StringFormat(__VA_ARGS__));   \
            _action;                                                                                                   \
        }
#else
    #define FASSERT_ACTION(_check, _action, ...)                                                                       \
        if (!(_check)) {                                                                                               \
            auto __debugInfo__ = Fundamental::StringFormat("[{}"                                                       \
                                                           "(" STR_HELPER(__LINE__) ")] [####check####:" #_check "] ", \
                                                           __func__);                                                  \
            Fundamental::Logger::s_defaultLogger->LogOutput(Fundamental::LogLevel::warn,                               \
                                                            __debugInfo__ + Fundamental::StringFormat(__VA_ARGS__));   \
            _action;                                                                                                   \
        }
#endif
// add  more logger instance
#define FINFO_I(logger, ...) FLOG(logger, Fundamental::LogLevel::info, ##__VA_ARGS__)
#define FERR_I(logger, ...)  FLOG_DEBUGINFO(logger, Fundamental::LogLevel::err, ##__VA_ARGS__)
#define FFAIL_I(logger, ...) FLOG_DEBUGINFO(logger, Fundamental::LogLevel::critical, ##__VA_ARGS__)
#define FWARN_I(logger, ...) FLOG_DEBUGINFO(logger, Fundamental::LogLevel::warn, ##__VA_ARGS__)

#ifdef DEBUG
    #define FDEBUGS_I(logger) Fundamental::LoggerStream(logger, Fundamental::LogLevel::debug).stream()
#else
    #define FDEBUGS_I(logger) Fundamental::LoggerStream(logger, Fundamental::LogLevel::debug).null_stream()
#endif
#define FINFOS_I(logger)  Fundamental::LoggerStream(logger, Fundamental::LogLevel::info).stream()
#define FINFOSL_I(logger) Fundamental::LoggerStream(logger, Fundamental::LogLevel::info, __LINE__).stream()
#define FERRS_I(logger)                                                                                                \
    Fundamental::LoggerStream(logger, Fundamental::LogLevel::err, LOG_FILE_NAME, __func__, __LINE__).stream()
#define FFAILS_I(logger)                                                                                               \
    Fundamental::LoggerStream(logger, Fundamental::LogLevel::critical, LOG_FILE_NAME, __func__, __LINE__).stream()
#define FWARNS_I(logger)                                                                                               \
    Fundamental::LoggerStream(logger, Fundamental::LogLevel::warn, LOG_FILE_NAME, __func__, __LINE__).stream()

/*end of file*/