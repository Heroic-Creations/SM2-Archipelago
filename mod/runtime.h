// Where the mod and the client meet on disk.
//
// Everything the two sides exchange -- commands, status, game state, logs --
// lives in %LOCALAPPDATA%\SM2-Archipelago. Both sides derive it; nothing is
// configured, and it exists on every Windows machine. Until 2026-09-06 every
// one of these paths was a literal D:\SM2-AP-Capture, which only existed on
// the machine the mod was written on.
#pragma once

#include <Windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

inline const std::filesystem::path& RuntimeDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path base;
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
            base = p;
            CoTaskMemFree(p);
        } else {
            const char* env = getenv("LOCALAPPDATA");
            base = env ? env : "C:\\Temp";
        }
        std::filesystem::path d = base / "SM2-Archipelago";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

// A file inside the runtime folder, as a narrow string for fopen_s and friends.
inline std::string RuntimeFile(const char* name) {
    return (RuntimeDir() / name).string();
}
