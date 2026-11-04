#include "common/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

#ifdef ENABLE_SPDLOG
#include <spdlog/spdlog.h>
#endif

namespace tcu::common {

namespace {
std::mutex  g_log_mtx;
std::string g_daemon_name = "tcu";
LogLevel    g_min_level   = LogLevel::Info;

LogLevel parseLevel(const std::string& s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;
}
} // namespace

void Logger::init(const std::string& daemon_name, const std::string& level_str) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_daemon_name = daemon_name;
    g_min_level   = parseLevel(level_str);
#ifdef ENABLE_SPDLOG
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [" + daemon_name + "] %v");
    switch (g_min_level) {
        case LogLevel::Trace: spdlog::set_level(spdlog::level::trace); break;
        case LogLevel::Debug: spdlog::set_level(spdlog::level::debug); break;
        case LogLevel::Info:  spdlog::set_level(spdlog::level::info); break;
        case LogLevel::Warn:  spdlog::set_level(spdlog::level::warn); break;
        case LogLevel::Error: spdlog::set_level(spdlog::level::err); break;
    }
#endif
}

void Logger::log(LogLevel lvl, std::string_view msg) {
    if (static_cast<int>(lvl) < static_cast<int>(g_min_level)) {
        return;
    }
#ifdef ENABLE_SPDLOG
    switch (lvl) {
        case LogLevel::Trace: spdlog::trace("{}", msg); break;
        case LogLevel::Debug: spdlog::debug("{}", msg); break;
        case LogLevel::Info:  spdlog::info("{}", msg); break;
        case LogLevel::Warn:  spdlog::warn("{}", msg); break;
        case LogLevel::Error: spdlog::error("{}", msg); break;
    }
#else
    std::lock_guard<std::mutex> lk(g_log_mtx);
    const char* tag = "INFO ";
    switch (lvl) {
        case LogLevel::Trace: tag = "TRACE"; break;
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info:  tag = "INFO "; break;
        case LogLevel::Warn:  tag = "WARN "; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }
    std::cerr << "[" << tag << "] [" << g_daemon_name << "] " << msg << "\n";
#endif
}

} // namespace tcu::common

