#pragma once

#if LOGLEVEL >= 4
    #define DEBUG(fmt, ...) logging::log(logging::LogLevel::DEBUG, fmt, __VA_ARGS__)
#else
    #define DEBUG(fmt, ...)
#endif

#if LOGLEVEL >= 3
    #define INFO(fmt, ...) logging::log(logging::LogLevel::INFO, fmt, __VA_ARGS__)
#else
    #define INFO(fmt, ...)
#endif

#if LOGLEVEL >= 2
    #define WARN(fmt, ...) logging::log(logging::LogLevel::WARN, fmt, __VA_ARGS__)
#else
    #define WARN(fmt, ...)
#endif

#if LOGLEVEL >= 1
    #define FATAL(fmt, ...) logging::log(logging::LogLevel::FATAL, fmt, __VA_ARGS__)
#else
    #define FATAL(fmt, ...)
#endif

// Headline channel: what the player sees on the live console. One plain line,
// no timestamp, no level tag. Also written to the file (tagged SAY) so the
// full record stays in one place. INFO/DEBUG detail goes to the file only.
#define SAY(fmt, ...) logging::say(fmt, __VA_ARGS__)

namespace logging {
    enum LogLevel {
        INFO,
        WARN,
        DEBUG,
        FATAL
    };

    void log(const LogLevel level, const char* fmt, ...);
    void say(const char* fmt, ...);
}