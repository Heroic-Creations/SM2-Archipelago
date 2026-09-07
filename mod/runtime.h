// Where the mod and the client meet on disk.
//
// Everything the two sides exchange -- commands, status, game state, logs --
// lives in %ProgramData%\SM2-Archipelago (the same convention as Archipelago's
// own C:\ProgramData\Archipelago). Both sides derive it; nothing is configured.
//
// Not AppData\Local, deliberately: the Microsoft Store build of Python -- the
// one Windows 11 hands out by default -- silently redirects everything it
// writes under AppData\Local into a private per-package copy, so a client
// running on it and a native mod reading the real folder never see each
// other's files. ProgramData is not redirected (probed 2026-09-07).
// Until 2026-09-06 every one of these paths was a literal dev folder that
// only existed on the machine the mod was written on.
#pragma once

#include <Windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

inline const std::filesystem::path& RuntimeDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path base;
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p)) && p) {
            base = p;
            CoTaskMemFree(p);
        } else {
            const char* env = getenv("ProgramData");
            base = env ? env : "C:\\ProgramData";
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
