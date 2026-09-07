// File-backed replacement for the template's console logger.
//
// The original wrote to a console via termcolor, which is invisible unless the
// game is launched with -console. We want the output on disk instead, since the
// whole point of this build is to capture a dump we can read afterwards.

#include "logging.h"
#include "runtime.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace {

const char* LevelName(logging::LogLevel level) {
    switch (level) {
    case logging::INFO:  return "INFO ";
    case logging::WARN:  return "WARN ";
    case logging::DEBUG: return "DEBUG";
    case logging::FATAL: return "FATAL";
    }
    return "?????";
}

// Resolved once -- GetModuleFileName per line would be wasteful for a big dump.
const std::filesystem::path& LogPath() {
    static const std::filesystem::path path = [] {
        char module_path[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LevelName), &self);
        GetModuleFileNameA(self, module_path, MAX_PATH);
        {
            // Same reasoning as the capture output: Overstrike wipes scripts\.
            std::error_code ec;
            const std::filesystem::path dir = RuntimeDir();
            std::filesystem::create_directories(dir, ec);
            if (!ec || std::filesystem::exists(dir)) return dir / "APMod.log";
        }
        return std::filesystem::path(module_path).parent_path() / "APMod.log";
    }();
    return path;
}

std::mutex g_write_lock;

// The live console the mod opens on first use. Visible when the game runs
// borderless/windowed or on a second monitor; watch-log.cmd tails the file
// otherwise. Caller holds g_write_lock.
void ConsoleWrite(const char* text, size_t len) {
    static HANDLE console = nullptr;
    if (!console) {
        if (AllocConsole()) {
            SetConsoleTitleA("SM2-Archipelago (live)");
            console = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (console && console != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteConsoleA(console, text, static_cast<DWORD>(len), &written, nullptr);
    }
}

} // namespace

void logging::log(const LogLevel level, const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    OutputDebugStringA(message);

    std::lock_guard<std::mutex> guard(g_write_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    const int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s %s\n",
                           t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, LevelName(level), message);

    if (FILE* f = fopen(LogPath().string().c_str(), "a")) {
        fputs(line, f);
        fclose(f);
    }

    // Live console: only warnings and fatals from this channel, flagged so
    // they stand out. INFO/DEBUG detail (hex dumps, native resolution) stays
    // in the file. Headlines come through say() below.
    if ((level == LogLevel::WARN || level == LogLevel::FATAL) && n > 0) {
        char flagged[2210];
        const int m = snprintf(flagged, sizeof(flagged), "  !! %s\n", message);
        if (m > 0) ConsoleWrite(flagged, static_cast<size_t>(m));
    }
}

// Plain line for the player: no timestamp, no level. Mirrored to the file as
// SAY so the full record still lives in one place.
void logging::say(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> guard(g_write_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (FILE* f = fopen(LogPath().string().c_str(), "a")) {
        fprintf(f, "[%02d:%02d:%02d.%03d] SAY   %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);
        fclose(f);
    }
    char line[2100];
    const int n = snprintf(line, sizeof(line), "%s\n", message);
    if (n > 0) ConsoleWrite(line, static_cast<size_t>(n));
}
