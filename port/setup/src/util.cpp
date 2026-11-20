#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

namespace util {

std::string Utf8(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

std::string Format(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string Gb(u64 bytes) {
    const double gb = double(bytes) / double(1ull << 30);
    if (gb >= 10)
        return Format("%.0f GB", gb);
    if (gb >= 1)
        return Format("%.1f GB", gb);
    return Format("%.0f MB", double(bytes) / double(1ull << 20));
}

fs::path ExePath() {
    wchar_t exe[1024];
    GetModuleFileNameW(nullptr, exe, 1024);
    return exe;
}
fs::path SetupDir() { return ExePath().parent_path(); }

std::wstring Env(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

const wchar_t* Sandbox() {
    static const std::wstring s = Env(L"XMD_SETUP_SANDBOX");
    return s.empty() ? nullptr : s.c_str();
}

fs::path LocalDataDir() {
    fs::path dir;
    if (Sandbox()) {
        dir = fs::path(Sandbox()) / "local";
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
            dir = fs::path(p) / "XMenDestinyPC";
        CoTaskMemFree(p);
        if (dir.empty())
            dir = SetupDir() / "setup_data";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

namespace {
std::mutex g_log_mutex;
fs::path g_log_file;
} // namespace

void SetLogFile(const fs::path& path) {
    std::lock_guard lk(g_log_mutex);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    g_log_file = path;
}

void Log(const std::string& line) {
    std::lock_guard lk(g_log_mutex);
    if (g_log_file.empty())
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::ofstream f(g_log_file, std::ios::app);
    f << Format("%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond) << line << "\n";
}

std::wstring Quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

int Run(const std::wstring& cmdline, const std::vector<wchar_t>* env, const fs::path& log, unsigned long priority,
        std::atomic<bool>* cancel, std::string* captured, const fs::path& cwd) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE, rd = nullptr, wr = nullptr;
    if (captured) {
        if (!CreatePipe(&rd, &wr, &sa, 0))
            return -1;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        out = wr;
    } else if (!log.empty()) {
        out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out == INVALID_HANDLE_VALUE ? nullptr : out;
    si.hStdError = si.hStdOutput;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | priority | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                                        env ? (void*)env->data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (captured) {
        CloseHandle(wr);
        if (started) {
            char buf[4096];
            DWORD n = 0;
            while (::ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n)
                captured->append(buf, n);
        }
        CloseHandle(rd);
    } else if (out != INVALID_HANDLE_VALUE) {
        CloseHandle(out);
    }
    if (!started)
        return -1;
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
        if (cancel && *cancel) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

std::vector<u8> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool WriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return bool(f);
}

u64 MTime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : u64(t.time_since_epoch().count());
}

u64 FolderBytes(const fs::path& dir) {
    u64 n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec))
            n += it->file_size(ec);
    return n;
}

Space DriveSpace(const fs::path& p) {
    Space s;
    std::error_code ec;
    fs::path at = fs::absolute(p, ec);
    while (!at.empty() && !fs::exists(at, ec) && at != at.parent_path())
        at = at.parent_path();
    ULARGE_INTEGER avail{}, total{};
    if (GetDiskFreeSpaceExW(at.c_str(), &avail, &total, nullptr)) {
        s.free = avail.QuadPart;
        s.total = total.QuadPart;
    }
    s.drive = Utf8(at.root_name().wstring());
    return s;
}

std::string Sha1(const fs::path& p) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream f(p, std::ios::binary);
        std::vector<char> buf(1 << 20);
        bool ok = bool(f);
        while (ok && f) {
            f.read(buf.data(), std::streamsize(buf.size()));
            if (f.gcount() > 0)
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(f.gcount()), 0);
        }
        u8 digest[20];
        if (ok && BCryptFinishHash(hash, digest, 20, 0) == 0)
            for (u8 b : digest)
                hex += Format("%02x", b);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

} // namespace util
