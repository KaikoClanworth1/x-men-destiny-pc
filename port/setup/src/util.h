// XMD Setup: small helpers shared by every part (paths, text, running programs, the log).
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace util {

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
inline std::string Utf8(const fs::path& p) { return Utf8(p.wstring()); }
std::string Lower(std::string s);
// "7.4 GB", "350 MB"
std::string Gb(u64 bytes);
std::string Format(const char* fmt, ...);

fs::path ExePath();
fs::path SetupDir();     // the folder this exe is in
fs::path LocalDataDir(); // %LOCALAPPDATA%\XMenDestinyPC (XMD_SETUP_SANDBOX=<folder>\local in test runs)
// Test runs set XMD_SETUP_SANDBOX=<folder>: shortcuts, the Apps entry and LocalAppData data go there instead of the
// real desktop / Start menu / registry.
const wchar_t* Sandbox();
std::wstring Env(const wchar_t* name);

// The setup's log (setup.log). Thread safe; SetLogFile may be called again to move it.
void SetLogFile(const fs::path& path);
void Log(const std::string& line);

// Runs a program hidden and waits. Its output goes to `log` (appended) or, with `captured`, into that string.
// Returns its exit code (-1: could not start). `cancel` set: the program is ended.
int Run(const std::wstring& cmdline, const std::vector<wchar_t>* env, const fs::path& log, unsigned long priority,
        std::atomic<bool>* cancel = nullptr, std::string* captured = nullptr, const fs::path& cwd = {});
std::wstring Quote(const fs::path& p);

std::vector<u8> ReadFile(const fs::path& p);
bool WriteText(const fs::path& p, const std::string& text);
std::string ReadText(const fs::path& p);
u64 MTime(const fs::path& p);
u64 FolderBytes(const fs::path& dir);

struct Space {
    u64 free = 0, total = 0;
    std::string drive; // "F:"
};
Space DriveSpace(const fs::path& p);

// SHA-1 of a file as lower-case hex (empty: unreadable).
std::string Sha1(const fs::path& p);

} // namespace util
