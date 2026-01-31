#include "log.h"
#include "filesystem_utils.hpp"
#include "utils.hpp"

#include "fundamental/basic/cxx_config_include.hpp"
#include <condition_variable>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <memory>
#include <thread>
#include <unordered_map>

#if TARGET_PLATFORM_WINDOWS
    #ifdef _MSC_VER
        #pragma warning(push)
        #pragma warning(disable : 4996 26498 26495 26819 26800)
    #endif
    #include "spdlog/sinks/windebug_sink.h"
    #ifdef _MSC_VER
        #pragma warning(pop)
        #pragma warning(disable : 4996 26498 26495 26819 26800)
    #endif
    #include <windows.h>

    #include <dbghelp.h>
    #include <iostream>

    #pragma comment(lib, "dbghelp.lib")
#endif
namespace Fundamental
{
namespace details
{
struct CalcFileNameHelper;
// stop default logger thread
struct LogGuard {
    LogGuard() { // default init
    }
    ~LogGuard() {
        Logger::Release();
    }
};
using namespace spdlog;
void PrepareLogdir(const std::string& logPath) {
    spdlog::drop_all();
    try {
        bool ret = std_fs::create_directories(logPath);
        if (!ret && !std_fs::is_directory(logPath)) {
            std::cout << "create directory " << logPath << " failed" << std::endl;
            std::abort();
        }
    } catch (const std::exception& e) {
        std::cout << "create directory " << logPath << " failed:" << e.what() << std::endl;
        std::abort();
    }
}
class NativeLogSink SPDLOG_FINAL : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    // create daily file sink which rotates on given time
    NativeLogSink(Logger* logger,
                  filename_t base_filename,
                  int rotation_hour,
                  int rotation_minute,
                  std::size_t logFileLimitSize,
                  std::int64_t logFileMaxExistedSec) :
    loggerRef(logger), m_baseFilename(std::move(base_filename)), m_logFileLimitSize(logFileLimitSize),
    m_logFileMaxExistedSec(logFileMaxExistedSec), m_rotationH(rotation_hour), m_rotationM(rotation_minute) {
        if (rotation_hour < 0 || rotation_hour > 23 || rotation_minute < 0 || rotation_minute > 59) {
            m_rotationH = 0;
            m_rotationM = 0;
        }
        auto path    = std_fs::path(m_baseFilename);
        m_outputDir  = path.parent_path().string();
        m_matchStr   = StringFormat("{}{}", R"((.*)_(\d{4})-(\d{2})-(\d{2})(.*))", path.extension().string());
        m_rotationTp = _next_rotation_tp();
        if (!m_baseFilename.empty()) ReOpenFile();
    }

    ~NativeLogSink() override {
        StopFileOutputThread();
    }

    void SetMsgCatcher(const LogMessageCatchFunc& catcher) {
        m_catcher = catcher;
    }

    void StartFileOutputThread() {
        if (m_baseFilename.empty()) return;
        if (m_bFileWriterWorking) return;
        m_pActualWriteCache  = &m_writeCache[0];
        m_bFileWriterWorking = true;
        start_locker.test_and_set();
        m_thread = std::make_unique<std::thread>(&NativeLogSink::Run, this);
        while (start_locker.test_and_set())
            ;
    }

    void StopFileOutputThread() {
        if (m_baseFilename.empty()) return;
        if (!m_bFileWriterWorking) return;
        m_bFileWriterWorking = false;
        if (m_thread && m_thread->joinable()) m_thread->join();
        m_thread.reset();
    }

protected:
    void _sink_it(const spdlog::details::log_msg& msg) override {
        if (m_catcher) m_catcher(static_cast<LogLevel>(msg.level), msg.formatted.data(), msg.formatted.size());
        if (!m_bFileWriterWorking) return;
        std::string data(msg.formatted.data(), msg.formatted.size());
        std::scoped_lock<std::mutex> locker(m_threadMutex);
        (*m_pActualWriteCache).emplace_back(std::move(data));
        m_threadCV.notify_one();
    }

    void _flush() override {
    }

    void Run() {
        Fundamental::Utils::SetThreadName("logger_thread");
        Fundamental::ScopeGuard g([&]() {
            m_fileHelper.flush();
            m_fileHelper.close();
        });
        std::unique_lock<std::mutex> locker(m_threadMutex);
        start_locker.clear();
        while (m_bFileWriterWorking) {
            if ((*m_pActualWriteCache).empty()) m_threadCV.wait_for(locker, std::chrono::milliseconds(20));
            if ((*m_pActualWriteCache).empty()) continue;
            // switch cache
            std::list<std::string>& m_handleList = *m_pActualWriteCache;
            m_pActualWriteCache = (m_pActualWriteCache == &m_writeCache[0]) ? &m_writeCache[1] : &m_writeCache[0];
            // release mutex
            locker.unlock();
            if (std::chrono::system_clock::now() >= m_rotationTp) {
                ReOpenFile();
                m_rotationTp = _next_rotation_tp();
            }
            for (auto& item : m_handleList)
                WriteMsg(item);
            m_handleList.clear();
            m_fileHelper.flush();
            if (m_logFileLimitSize > 0 && m_writtenSize >= m_logFileLimitSize) {
                ReOpenFile();
            }
            // acquire mutex
            locker.lock();
        }
        // fllush msgs
        if (m_pActualWriteCache == &m_writeCache[0]) {
            for (auto& item : m_writeCache[1])
                WriteMsg(item);
            for (auto& item : m_writeCache[0])
                WriteMsg(item);
        } else {
            for (auto& item : m_writeCache[0])
                WriteMsg(item);
            for (auto& item : m_writeCache[1])
                WriteMsg(item);
        }
        m_writeCache[0].clear();
        m_writeCache[1].clear();
    }

private:
    void ReOpenFile() {
        try {
            m_fileHelper.close();
            if (!m_lastLogFileName.empty() && m_logFileLimitSize > 0) { // rename log filename
                filename_t basename, ext;
                std::tie(basename, ext) = spdlog::details::file_helper::split_by_extenstion(m_lastLogFileName);
                std::conditional<std::is_same<filename_t::value_type, char>::value, fmt::MemoryWriter,
                                 fmt::WMemoryWriter>::type w;
                w.write(SPDLOG_FILENAME_T("{}-{}{}"), basename, m_fileIndex, ext);
                auto newName = w.str();
                try {
                    std_fs::rename(m_lastLogFileName, newName);
                } catch (const std::exception& e) {
                    std::cerr << "rename " << m_lastLogFileName << " to " << newName << " failed " << e.what()
                              << std::endl;
                }
                ++m_fileIndex;
                m_writtenSize = 0;
            }
            auto newFileName = CalcFilename(m_baseFilename);

            if (newFileName != m_lastLogFileName) { // reset file status
                m_fileIndex       = 1;
                m_writtenSize     = 0;
                m_lastLogFileName = newFileName;
            }
            if (m_logFileMaxExistedSec > 0) {
                DistcleanLogPath();
            }
            m_fileHelper.open(newFileName);
        } catch (const std::exception& e) {
            auto errorHandler = loggerRef->errorHandler;
            if (errorHandler) errorHandler(e.what());
        }
    }

    void DistcleanLogPath() {
        Fundamental::fs::RemoveExpiredFiles(m_outputDir, m_matchStr, m_logFileMaxExistedSec, false);
    }

    void WriteMsg(const std::string& msg) {
        try {
            m_fileHelper.write(msg);
            m_writtenSize += msg.size();
        } catch (const std::exception& e) {
            auto errorHandler = loggerRef->errorHandler;
            if (errorHandler) errorHandler(e.what());
        }
    }
    static filename_t CalcFilename(const filename_t& filename) {
        std::tm tm = spdlog::details::os::localtime();
        filename_t basename, ext;
        std::tie(basename, ext) = spdlog::details::file_helper::split_by_extenstion(filename);
        std::conditional<std::is_same<filename_t::value_type, char>::value, fmt::MemoryWriter, fmt::WMemoryWriter>::type
            w;
        w.write(SPDLOG_FILENAME_T("{}_{:04d}-{:02d}-{:02d}{}"), basename, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                ext);
        return w.str();
    }

    std::chrono::system_clock::time_point _next_rotation_tp() {
        auto now           = std::chrono::system_clock::now();
        time_t tnow        = std::chrono::system_clock::to_time_t(now);
        tm date            = spdlog::details::os::localtime(tnow);
        date.tm_hour       = m_rotationH;
        date.tm_min        = m_rotationM;
        date.tm_sec        = 0;
        auto rotation_time = std::chrono::system_clock::from_time_t(std::mktime(&date));
        if (rotation_time > now) {
            return rotation_time;
        }
        return { rotation_time + std::chrono::hours(24) };
    }
    Logger* const loggerRef;

    filename_t m_baseFilename;
    std::string m_outputDir;
    std::string m_matchStr;

    std::size_t m_logFileLimitSize      = 0;
    std::int64_t m_logFileMaxExistedSec = 0;
    int m_rotationH;
    int m_rotationM;
    std::chrono::system_clock::time_point m_rotationTp;
    spdlog::details::file_helper m_fileHelper;
    filename_t m_lastLogFileName;
    std::size_t m_fileIndex   = 1;
    std::size_t m_writtenSize = 0;

    bool m_bFileWriterWorking = false;
    std::mutex m_threadMutex;
    std::condition_variable m_threadCV;
    std::list<std::string>* m_pActualWriteCache = nullptr;
    std::list<std::string> m_writeCache[2];
    std::unique_ptr<std::thread> m_thread;
    std::atomic_flag start_locker = ATOMIC_FLAG_INIT;
    LogMessageCatchFunc m_catcher;
};

} // namespace details
// init static resource
Logger* Logger::s_defaultLogger = new Logger();
static details::LogGuard s_guard;

Logger::Logger(const std::string_view& format_str) :
nativeLogSink(nullptr),
loggerStorage(spdlog::stdout_color_st(std::to_string(reinterpret_cast<std::uint64_t>(this)) + ("console"))),
loggerStderrStorage(spdlog::stdout_color_st(std::to_string(reinterpret_cast<std::uint64_t>(this)) + ("console_err"))) {
    loggerStorage->set_level(spdlog::level::level_enum::trace);
    loggerStderrStorage->set_level(spdlog::level::level_enum::warn);
    if (!format_str.empty()) {
        loggerStorage->set_pattern(std::string(format_str));
        loggerStderrStorage->set_pattern(std::string(format_str));
    }
}

Logger::~Logger() {
    spdlog::drop(loggerStorage->name());
    spdlog::drop(loggerStderrStorage->name());
}

void Logger::Initialize(LoggerInitOptions options, Logger* logger) {
    try {
        std::list<spdlog::sink_ptr> initList;
        std::list<spdlog::sink_ptr> initErrList;
        if (options.enableConsoleOutput) {
#if TARGET_PLATFORM_WINDOWS
            initList.emplace_back(std::make_shared<spdlog::sinks::wincolor_stdout_sink_mt>());
            initList.emplace_back(std::make_shared<spdlog::sinks::windebug_sink_mt>());
            initErrList.emplace_back(std::make_shared<spdlog::sinks::wincolor_stderr_sink_mt>());
            initErrList.emplace_back(std::make_shared<spdlog::sinks::windebug_sink_mt>());
#elif TARGET_PLATFORM_LINUX
            initList.emplace_back(std::make_shared<spdlog::sinks::ansicolor_stdout_sink_st>());
            initErrList.emplace_back(std::make_shared<spdlog::sinks::ansicolor_stderr_sink_st>());
#endif
        }
        std::string fileName;
        auto logOutputPath = options.logOutputPath.value_or(LoggerInitOptions::kDefaultLogOutputPath);
        auto logOutputProgramName =
            options.logOutputProgramName.value_or(LoggerInitOptions::kDefaultLogOutputProgramName);
        if (!logOutputPath.empty() && !logOutputProgramName.empty()) {
            details::PrepareLogdir(logOutputPath);
            fileName = logOutputPath + "/" + logOutputProgramName + "_" + std::to_string(Utils::GetProcessId()) +
                       options.logOutputFileExt.value_or(LoggerInitOptions::kDefaultLogOutputFileExt);
        }
        if (!fileName.empty()) { // init file writer
            logger->nativeLogSink = std::make_shared<details::NativeLogSink>(
                logger, fileName, options.rotationHour.value_or(LoggerInitOptions::kDefaultRotationHour),
                options.rotationMin.value_or(LoggerInitOptions::kDefaultRotationMin),
                options.logFileLimitSize.value_or(LoggerInitOptions::kDefaultLogFileLimitSize),
                options.maxLogReserveTimeSec.value_or(LoggerInitOptions::kDefaultMaxLogReserveTimeSec));
            logger->nativeLogSink->SetMsgCatcher(options.catchHandler);
            logger->nativeLogSink->StartFileOutputThread();
            initList.emplace_back(logger->nativeLogSink);
            initErrList.emplace_back(logger->nativeLogSink);
        }
        auto prefer_log_level = options.minimumLevel.value_or(LoggerInitOptions::kDefaultLogLevel);
        if (initList.empty()) {
            prefer_log_level = Fundamental::LogLevel::off;
        }
        std::string logger_name =
            options.loggerName.empty() ? LoggerInitOptions::kDefaultCustomLoggerName : options.loggerName;
        {
            if (logger->loggerStorage) spdlog::drop(logger->loggerStorage->name());
            logger->loggerStorage = spdlog::create(logger_name, initList.begin(), initList.end());
            logger->loggerStorage->set_level(static_cast<spdlog::level::level_enum>(prefer_log_level));
            logger->loggerStorage->set_pattern(options.logFormat.value_or(LoggerInitOptions::kDefaultLogFormat));
            if (options.errorHandler) {
                logger->loggerStorage->set_error_handler(options.errorHandler);
            }
        }
        {
            if (logger->loggerStderrStorage) spdlog::drop(logger->loggerStderrStorage->name());
            logger->loggerStderrStorage = spdlog::create(logger_name + "_err", initErrList.begin(), initErrList.end());
            logger->loggerStderrStorage->set_level(static_cast<spdlog::level::level_enum>(prefer_log_level));
            logger->loggerStderrStorage->set_pattern(options.logFormat.value_or(LoggerInitOptions::kDefaultLogFormat));
            if (options.errorHandler) {
                logger->loggerStderrStorage->set_error_handler(options.errorHandler);
            }
        }
        logger->errorHandler = options.errorHandler;
    } catch (const std::exception& e) {
        std::cerr << " log initialize failed!" << e.what() << std::endl;
    }
}

void Logger::Release(Logger* logger) {
    if (logger->nativeLogSink) logger->nativeLogSink->StopFileOutputThread();
}

bool Logger::IsDebuggerAttached() {
#if TARGET_PLATFORM_WINDOWS
    return ::IsDebuggerPresent();
#else
    // alwasy return true
    return true;
#endif
}

void Logger::PrintBackTrace() {
#if TARGET_PLATFORM_WINDOWS
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    CONTEXT context      = {};
    context.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&context);

    SymInitialize(process, NULL, TRUE);

    STACKFRAME64 stack = {};
    DWORD imageType    = IMAGE_FILE_MACHINE_I386;

    #ifdef _M_IX86
    imageType              = IMAGE_FILE_MACHINE_I386;
    stack.AddrPC.Offset    = context.Eip;
    stack.AddrPC.Mode      = AddrModeFlat;
    stack.AddrFrame.Offset = context.Ebp;
    stack.AddrFrame.Mode   = AddrModeFlat;
    stack.AddrStack.Offset = context.Esp;
    stack.AddrStack.Mode   = AddrModeFlat;
    #elif _M_X64
    imageType              = IMAGE_FILE_MACHINE_AMD64;
    stack.AddrPC.Offset    = context.Rip;
    stack.AddrPC.Mode      = AddrModeFlat;
    stack.AddrFrame.Offset = context.Rsp;
    stack.AddrFrame.Mode   = AddrModeFlat;
    stack.AddrStack.Offset = context.Rsp;
    stack.AddrStack.Mode   = AddrModeFlat;
    #endif

    for (size_t i = 0; i < 62; ++i) {
        BOOL result = StackWalk64(imageType, process, thread, &stack, &context, NULL, SymFunctionTableAccess64,
                                  SymGetModuleBase64, NULL);

        if (!result) {
            break;
        }

        DWORD64 displacement      = 0;
        IMAGEHLP_SYMBOL64* symbol = (IMAGEHLP_SYMBOL64*)malloc(sizeof(IMAGEHLP_SYMBOL64) + 256);
        if (!symbol) break;
        symbol->SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL64);
        symbol->MaxNameLength = 255;

        if (SymGetSymFromAddr64(process, stack.AddrPC.Offset, &displacement, symbol)) {
            std::cout << "pc[" << stack.AddrPC.Offset << "]: " << symbol->Name << std::endl;
        }

        free(symbol);
    }

    SymCleanup(process);
#endif
}

void Logger::TestLogInstance() {
    std::cout << "internal log instance " << s_defaultLogger->loggerStorage.get() << std::endl;
    std::cout << "spd log storage " << &spdlog::details::registry::instance() << std::endl;
}

LoggerStream::LoggerStream(Logger* logger, LogLevel level) : loggerRef(logger), level_(level) {
}
LoggerStream::LoggerStream(Logger* logger,
                           LogLevel level,
                           const char* fileName,
                           const char* funcName,
                           std::int32_t line) : loggerRef(logger), level_(level) {
#if defined(__LOG_PRINT_FILE_NAME__)
    ss_ << Fundamental::StringFormat("[{}:{}({})] ", fileName, funcName, line);
#else
    ss_ << Fundamental::StringFormat("[{}({})] ", funcName, line);
#endif
}
LoggerStream::LoggerStream(Logger* logger, LogLevel level, std::int32_t line) : loggerRef(logger), level_(level) {
    ss_ << Fundamental::StringFormat("[{}] ", line);
}
LoggerStream::~LoggerStream() {
#ifndef DISABLE_FLOG
    if (enable_output) loggerRef->Logger::LogOutput(level_, ss_.str());
#endif
}
#if ENABLE_FUNDAMENTAL_ASSERT_MACRO
    #pragma message("fundamental assert macro was enabled")
#else
    #pragma message("fundamental assert macro was disabled")
#endif
} // namespace Fundamental