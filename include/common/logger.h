#pragma once

#include <string>
#include <string_view>

namespace tcu::common {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

class Logger {
public:
    static void init(const std::string& daemon_name, const std::string& level_str = "info");
    static void log(LogLevel lvl, std::string_view msg);

    static void info(std::string_view msg)  { log(LogLevel::Info, msg); }
    static void warn(std::string_view msg)  { log(LogLevel::Warn, msg); }
    static void error(std::string_view msg) { log(LogLevel::Error, msg); }
    static void debug(std::string_view msg) { log(LogLevel::Debug, msg); }
};

} // namespace tcu::common

