#include "builder.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <windows.h>
#include <shlobj.h>

#include "toolchain.h"
#include "xiso.h"

namespace builder {

using util::Format;
using util::Quote;
using util::Run;
using util::Utf8;

const char* const kStepNames[kStepCount] = {"Read your game files", "Translate the game to PC code", "Compile",
                                            "Put the game together", "Prepare the shaders"};
const float kStepWeights[kStepCount] = {0.20f, 0.06f, 0.44f, 0.05f, 0.25f};

namespace {

// A file only this game's disc has: tells "another version of this game" from "another game".
constexpr const char* kMarkerFile = "xmd_common.bnk";
// Written into the install folder by the game, the tests or an earlier setup: never copied from a source folder.
constexpr const char* kNotGameData[] = {"userdata", "logs", "cmdbuf", "crashes", "captures", "cache", "native_shaders"};

bool IsNotGameData(const fs::path& rel) {
    const std::string top = util::Lower(Utf8(*rel.begin()));
    for (const char* n : kNotGameData)
        if (top == n)
            return true;
    const std::string ext = util::Lower(Utf8(rel.extension()));
    const std::string name = util::Lower(Utf8(rel.filename()));
    return ext == ".exe" || ext == ".dll" || ext == ".pdb" || ext == ".log" || ext == ".dmp" || ext == ".toml" ||
           ext == ".ini" || ext == ".lnk" || ext == ".ico" || name.rfind("unins", 0) == 0;
}

std::string Ini(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.rfind(key + "=", 0) == 0)
            return line.substr(key.size() + 1);
    }
    return {};
}

std::string Fwd(const fs::path& p) { // forward slashes: clang's response files and TOML read backslashes as escapes
    std::string s = Utf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}
std::string QF(const fs::path& p) { return "\"" + Fwd(p) + "\""; }

} // namespace

// ------------------------------------------------------------------------------------------------ the player's copy

std::string GameSource::Describe() const {
    return std::string(iso ? "Disc image" : "Game folder") + (bytes ? "  \xC2\xB7  " + util::Gb(bytes) : std::string());
}
const char* GameSource::Badge() const {
    switch (verdict) {
    case Verdict::Supported: return "Supported version";
    case Verdict::WrongVersion: return "Wrong version";
    case Verdict::NotThisGame: return "Not this game";
    default: return "Cannot be read";
    }
}

Sdk LoadSdk(const fs::path& dir) {
    Sdk s;
    s.dir = dir;
    const std::string ini = util::ReadText(dir / "sdk.ini");
    s.version = Ini(ini, "version");
    s.min_msvc_minor = std::atoi(Ini(ini, "min_msvc_minor").c_str());
    std::istringstream sha(Ini(ini, "xex_sha1"));
    for (std::string h; std::getline(sha, h, ',');)
        if (!h.empty())
            s.xex_sha1.push_back(util::Lower(h));
    std::error_code ec;
    for (const char* need : {"sdk.ini", "bin/rexglue.exe", "codegen/xmd_config.toml", "include/rexglue/rex/ppc.h",
                             "lib/rexruntime.lib", "obj/main.cpp.obj", "bin/rexruntime.dll"})
        if (!fs::exists(dir / need, ec)) {
            s.problem = "This setup is incomplete (" + std::string(need) + " is missing from its sdk folder).";
            break;
        }
    if (s.problem.empty() && s.xex_sha1.empty())
        s.problem = "This setup is incomplete (sdk.ini names no game version).";
    return s;
}

GameSource Probe(const fs::path& path, const fs::path& sdk_dir) {
    GameSource g;
    g.path = path;
    const Sdk sdk = LoadSdk(sdk_dir);
    std::error_code ec;
    std::string sha;
    bool marker = false;
    if (fs::is_directory(path, ec)) {
        if (!fs::exists(path / "Default.xex", ec)) {
            g.verdict = Verdict::NotThisGame;
            g.detail = "This folder has no Default.xex: choose the folder the disc was extracted to.";
            return g;
        }
        sha = util::Sha1(path / "Default.xex");
        marker = fs::exists(path / kMarkerFile, ec);
        for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (IsNotGameData(it->path().filename()))
                continue;
            g.bytes += it->is_directory(ec) ? util::FolderBytes(it->path()) : it->file_size(ec);
        }
    } else {
        g.iso = true;
        xiso::Image img;
        if (!xiso::Open(path, img, g.detail)) {
            g.verdict = fs::exists(path, ec) ? Verdict::NotThisGame : Verdict::Unreadable;
            return g;
        }
        g.bytes = img.total;
        const xiso::Entry* xex = img.Find("Default.xex");
        if (!xex) {
            g.verdict = Verdict::NotThisGame;
            g.detail = "This disc image has no Default.xex.";
            return g;
        }
        sha = xiso::Sha1(img, *xex);
        marker = img.Find(kMarkerFile) != nullptr;
    }
    if (sha.empty()) {
        g.verdict = Verdict::Unreadable;
        g.detail = "Default.xex could not be read.";
    } else if (std::find(sdk.xex_sha1.begin(), sdk.xex_sha1.end(), sha) != sdk.xex_sha1.end()) {
        g.verdict = Verdict::Supported;
    } else if (marker) {
        g.verdict = Verdict::WrongVersion;
        g.detail = "This is X-Men: Destiny, but another version of it (another region, or a changed Default.xex) "
                   "than the one this port was made for.";
    } else {
        g.verdict = Verdict::NotThisGame;
        g.detail = "This is another game.";
    }
    return g;
}

std::vector<fs::path> FindGameSources(std::atomic<bool>& cancel) {
    std::vector<fs::path> folders;
    const fs::path here = util::SetupDir();
    for (const fs::path& p : {here, here / "Isos", here / "isos", here / "Games", here.parent_path(), here.parent_path() / "Isos"})
        folders.push_back(p);
    for (const KNOWNFOLDERID* id : {&FOLDERID_Downloads, &FOLDERID_Desktop, &FOLDERID_Documents}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p)))
            folders.push_back(p);
        CoTaskMemFree(p);
    }
    wchar_t drives[512];
    const DWORD n = GetLogicalDriveStringsW(511, drives);
    for (const wchar_t* d = drives; d < drives + n && *d; d += wcslen(d) + 1)
        if (GetDriveTypeW(d) == DRIVE_FIXED)
            for (const char* name : {"Games", "Xbox 360", "Xbox360", "X360", "Isos", "ISO", "ROMs", "Roms", "Xbox Games", "Emulators"})
                folders.push_back(fs::path(d) / name);

    auto named = [](const fs::path& p) {
        std::string n = util::Lower(Utf8(p.filename()));
        n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' ' || c == '-' || c == '_' || c == '.'; }), n.end());
        return n.find("xmen") != std::string::npos && n.find("destiny") != std::string::npos;
    };
    std::vector<fs::path> found;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(p, ec);
        if (std::find(found.begin(), found.end(), c) == found.end())
            found.push_back(c);
    };
    std::error_code ec;
    for (const fs::path& top : folders) {
        if (cancel)
            break;
        if (!fs::is_directory(top, ec))
            continue;
        if (fs::exists(top / "Default.xex", ec) && fs::exists(top / kMarkerFile, ec))
            add(top);
        int seen = 0;
        for (fs::recursive_directory_iterator it(top, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && !cancel && seen < 4000; it.increment(ec), seen++) {
            if (it.depth() >= 2)
                it.disable_recursion_pending();
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                if (fs::exists(p / "Default.xex", ec) && fs::exists(p / kMarkerFile, ec)) {
                    add(p);
                    it.disable_recursion_pending();
                }
            } else if (util::Lower(Utf8(p.extension())) == ".iso" && named(p)) {
                add(p);
            }
        }
    }
    return found;
}

// ------------------------------------------------------------------------------------------------------ progress

void Progress::Log(std::string s) {
    util::Log(s);
    std::lock_guard lk(mutex);
    log.push_back(std::move(s));
    if (log.size() > 400)
        log.erase(log.begin(), log.begin() + 100);
}
std::string Progress::GetError() {
    std::lock_guard lk(mutex);
    return error;
}
std::string Progress::GetWarning() {
    std::lock_guard lk(mutex);
    return warning;
}
std::vector<std::string> Progress::Tail(size_t n) {
    std::lock_guard lk(mutex);
    return {log.end() - std::min(n, log.size()), log.end()};
}
float Progress::Overall() const {
    if (finished && ok)
        return 1.0f;
    float done_w = 0;
    const int s = std::clamp(step.load(), 0, int(kStepCount) - 1);
    for (int i = 0; i < s; i++)
        done_w += kStepWeights[i];
    return std::min(0.999f, done_w + kStepWeights[s] * std::clamp(fraction.load(), 0.0f, 1.0f));
}

int DefaultJobs() {
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    const int by_ram = std::max(1, int(ms.ullTotalPhys / (2048ull << 20)));
    const int cores = std::max(1, int(std::thread::hardware_concurrency()) - 1);
    return std::clamp(std::min(cores, by_ram), 1, 32);
}

// --------------------------------------------------------------------------------------------------------- build

fs::path SavedShadersDir(const fs::path& install_dir) { return install_dir / "UserData" / "cache" / "shaders_new"; }

namespace {

// Runs the release's shader tool (sdk\shadertools\prepare_shaders.py with the Python packed beside it): finds the
// game's shaders, converts them, writes the packs into <install>\native_shaders. Shaders the running game saved
// (SavedShadersDir) are added. Returns why it failed, or nothing.
std::string RunShaderTool(const Paths& p, int jobs, Progress& prog, const std::vector<wchar_t>& env, float from = 0.0f, float to = 1.0f) {
    std::error_code ec;
    const fs::path tools = p.sdk_dir / "shadertools";
    const fs::path work = p.work_dir / "shaders";
    // the tools' environment: the C++ tools' when there are any (fxc is in their Windows SDK), with the game folder
    // first on PATH (Microsoft's C++ runtime, which the converter needs, is there)
    const std::vector<wchar_t> tool_env = toolchain::WithPathFirst(env, p.install_dir);
    const fs::path slog = p.work_dir / "shaders.log";
    fs::create_directories(p.work_dir, ec);
    fs::remove(slog, ec);
    std::atomic<bool> watching{true};
    std::thread watch([&] { // the tool's last "progress <step> <done> <total>" line
        while (watching) {
            const std::string text = util::ReadText(slog);
            const size_t at = text.rfind("progress ");
            char step[32] = "";
            int done = 0, total = 0;
            if (at != std::string::npos && std::sscanf(text.c_str() + at, "progress %31s %d %d", step, &done, &total) == 3 && total > 0) {
                const float f = float(done) / float(total);
                const bool find = std::string(step) == "find";
                prog.fraction = from + (to - from) * (find ? 0.12f * f : 0.12f + 0.86f * f);
                prog.shaders_done = find ? 0 : done;
                prog.shaders_total = find ? 0 : total;
            }
            Sleep(300);
        }
    });
    std::wstring cmd = Quote(tools / "python" / "python.exe") + L" -I " + Quote(tools / "prepare_shaders.py") + L" " +
                       Quote(p.install_dir) + L" " + Quote(work) + L" " + Quote(p.install_dir / "native_shaders") +
                       L" --jobs=" + std::to_wstring(std::max(1, jobs));
    if (fs::exists(SavedShadersDir(p.install_dir), ec))
        cmd += L" " + Quote(L"--extra=" + SavedShadersDir(p.install_dir).wstring());
    const int rc = Run(cmd, &tool_env, slog, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel, nullptr, tools);
    watching = false;
    watch.join();
    if (prog.cancel)
        return "Cancelled.";
    if (rc == 0)
        return {};
    std::string why = Format("the converter stopped with code %d", rc);
    const std::string text = util::ReadText(slog);
    if (const size_t at = text.rfind("error "); at != std::string::npos)
        why = text.substr(at + 6, text.find_first_of("\r\n", at) - at - 6);
    return why + "; see " + Utf8(slog);
}

// One start of the built game that nobody sees or hears, the way the port's own tests run it: sound off, the window
// minimised without taking the keyboard and moved below every screen, low priority, no input. The game saves the
// shaders it compiles (the port's renderer does that whenever it has no converted one). It is ended when no new
// one came for 20 seconds (they are all made as it starts), when XMD_COLLECT_SHADERS makes it end itself, or
// after three minutes. Returns how many shaders are waiting to be converted.
std::vector<wchar_t> CurrentEnvironment();

int CollectStartupShaders(const Paths& p, Progress& prog, const std::vector<wchar_t>& env) {
    std::error_code ec;
    const fs::path exe = p.install_dir / "xmd.exe";
    // (this program's own environment, as when the player starts the game - not the C++ tools')
    (void)env;
    std::vector<wchar_t> e = CurrentEnvironment();
    // (XMD_AFFINITY: every CPU, said outright as the port's tests do)
    DWORD_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    wchar_t mask[32];
    swprintf(mask, 32, L"%llX", static_cast<unsigned long long>(system_mask ? system_mask : 0xFFFF));
    e = toolchain::WithVar(e, L"XMD_AFFINITY", mask);
    for (const auto& [name, value] : {std::pair{L"XMD_COLLECT_SHADERS", L"1"}, std::pair{L"XMD_AUDIO_MUTE", L"1"},
                                      std::pair{L"SVR2011_WINDOW_BEHIND", L"1"}, std::pair{L"SDL_WINDOW_ACTIVATE_WHEN_SHOWN", L"0"},
                                      std::pair{L"SDL_WINDOW_ACTIVATE_WHEN_RAISED", L"0"}})
        e = toolchain::WithVar(e, name, value);
    // (its own settings file and log, so the player's are not made by this start)
    e = toolchain::WithVar(e, L"XMD_CONFIG", (p.work_dir / "collect_config.toml").wstring());
    std::wstring cmd = Quote(exe) + L" --log_file=" + Quote(p.work_dir / "collect.log") +
                       L" --log_level=info --audio_mute=true --fullscreen=false --mnk_mode=false --vsync=false"
                       L" --d3d12_allow_variable_refresh_rate_and_tearing=false --native_renderer=main --gpu_backend=d3d12";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
                        e.data(), p.install_dir.c_str(), &si, &pi)) {
        prog.Log(Format("the game could not be started (error %lu)", GetLastError()));
        return 0;
    }
    CloseHandle(pi.hThread);
    struct Park {
        DWORD pid;
        static BOOL CALLBACK Each(HWND w, LPARAM self) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == reinterpret_cast<Park*>(self)->pid && GetWindow(w, GW_OWNER) == nullptr) {
                const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int bottom = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN);
                SetWindowPos(w, HWND_BOTTOM, left + 40, bottom + 200, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return TRUE;
        }
    } park{pi.dwProcessId};
    const auto start = std::chrono::steady_clock::now();
    auto last_new = start;
    int count = SavedShaderCount(p.install_dir);
    const int before = count;
    while (WaitForSingleObject(pi.hProcess, 250) == WAIT_TIMEOUT) {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - start).count();
        if (secs < 60)
            EnumWindows(&Park::Each, reinterpret_cast<LPARAM>(&park)); // (its windows appear over the first seconds)
        const int n = SavedShaderCount(p.install_dir);
        if (n != count)
            count = n, last_new = now;
        const double quiet = std::chrono::duration<double>(now - last_new).count();
        prog.fraction = 0.86f + 0.07f * float(std::min(1.0, secs / 60.0));
        if (prog.cancel || secs > 180 || (count > before && quiet > 20) || (count == before && secs > 90)) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 10000);
            break;
        }
    }
    CloseHandle(pi.hProcess);
    fs::remove(p.work_dir / "collect_config.toml", ec);
    return SavedShaderCount(p.install_dir);
}

std::vector<wchar_t> CurrentEnvironment() {
    std::vector<wchar_t> block;
    if (wchar_t* e = GetEnvironmentStringsW()) {
        const wchar_t* end = e;
        while (*end)
            end += wcslen(end) + 1;
        block.assign(static_cast<const wchar_t*>(e), end + 1);
        FreeEnvironmentStringsW(e);
    } else {
        block.assign(2, 0);
    }
    return block;
}

} // namespace

int SavedShaderCount(const fs::path& install_dir) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(SavedShadersDir(install_dir), ec))
        if (util::Lower(Utf8(e.path().extension())) == ".xsc")
            n++;
    return n;
}

bool AddSavedShaders(const Paths& p, int jobs, Progress& prog, std::string& why) {
    prog.running = true;
    prog.finished = false;
    prog.ok = false;
    prog.step = kShaders;
    prog.fraction = 0;
    std::error_code ec;
    if (!fs::exists(p.sdk_dir / "shadertools" / "prepare_shaders.py", ec) || !fs::exists(p.sdk_dir / "shadertools" / "python" / "python.exe", ec)) {
        why = "the setup package this game was built with is gone";
    } else {
        std::vector<wchar_t> env;
        const toolchain::Toolchain tc = toolchain::Find(0);
        if (!tc.ok || !toolchain::CaptureEnvironment(tc.vcvars, env))
            env = CurrentEnvironment(); // (no C++ tools any more: everything but Direct3D 11's shaders still converts)
        why = RunShaderTool(p, jobs, prog, env);
    }
    prog.Log(why.empty() ? "saved shaders added" : "saved shaders: " + why);
    prog.ok = why.empty();
    prog.running = false;
    prog.finished = true;
    return why.empty();
}

int BuildGame(const Paths& p, int jobs, Progress& prog) {
    prog.running = true;
    prog.finished = false;
    prog.ok = false;
    prog.step = kRead;
    prog.fraction = 0;
    prog.done = 0;
    prog.total = 0;
    std::error_code ec;
    fs::create_directories(p.work_dir, ec);
    fs::create_directories(p.install_dir, ec);
    const fs::path log = p.work_dir / "build.log";
    fs::remove(log, ec);
    auto note = [&](const std::string& s) {
        prog.Log(s);
        std::ofstream(log, std::ios::app) << s << "\n";
    };
    auto fail = [&](int code, std::string why) {
        {
            std::lock_guard lk(prog.mutex);
            prog.error = why;
        }
        note("FAILED: " + why);
        prog.running = false;
        prog.finished = true;
        return code;
    };
    auto stage = [&](Step s) {
        prog.step = s;
        prog.fraction = 0;
        note(std::string("== ") + kStepNames[s]);
    };
    const auto t0 = std::chrono::steady_clock::now();
    const Sdk sdk = LoadSdk(p.sdk_dir);
    if (!sdk.problem.empty())
        return fail(4, sdk.problem);
    note("XMD Setup " + sdk.version + ": building into " + Utf8(p.install_dir));

    // the C++ tools first: nothing is written when they are missing
    const toolchain::Toolchain tc = toolchain::Find(sdk.min_msvc_minor);
    if (!tc.Ready())
        return fail(3, tc.problem.empty() ? "The C++ tools were not found." : tc.problem);
    std::vector<wchar_t> env;
    if (!toolchain::CaptureEnvironment(tc.vcvars, env))
        return fail(3, "Could not set up the C++ tools (vcvars64.bat).");
    note("C++ tools " + tc.version + ", clang " + tc.clang_version + " at " + Utf8(tc.clang));
    // Microsoft's C++ runtime the game (and rexglue.exe) uses, from the player's own Visual Studio (its
    // redistributable folder), else the one Windows has.
    auto copy_crt = [&](const fs::path& to) {
        std::vector<fs::path> crt_dirs;
        const fs::path redist = toolchain::EnvValue(env, L"VCToolsRedistDir");
        if (!redist.empty())
            for (auto& e : fs::directory_iterator(redist / "x64", ec))
                if (Utf8(e.path().filename()).find(".CRT") != std::string::npos)
                    crt_dirs.push_back(e.path());
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        crt_dirs.push_back(sys);
        for (const char* n : {"msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"})
            for (const fs::path& d : crt_dirs)
                if (fs::exists(d / n, ec)) {
                    CopyFileW((d / n).c_str(), (to / n).c_str(), FALSE);
                    break;
                }
    };

    // ---- 1. the game's files, into the install folder
    stage(kRead);
    const GameSource src = Probe(p.source, p.sdk_dir);
    if (src.verdict != Verdict::Supported)
        return fail(2, std::string(src.Badge()) + ": " + Utf8(p.source) + (src.detail.empty() ? "" : " - " + src.detail));
    {
        const util::Space space = util::DriveSpace(p.install_dir);
        const u64 have = util::FolderBytes(p.install_dir);
        if (space.free + have < src.bytes + kProgramBytes)
            return fail(4, "Not enough free space on " + space.drive + " (" + util::Gb(src.bytes + kProgramBytes) + " needed).");
    }
    prog.bytes_total = src.bytes;
    auto on_bytes = [&](u64 done, u64 total) {
        prog.bytes_done = done;
        prog.fraction = total ? float(double(done) / double(total)) : 1.0f;
    };
    std::string placed; // the game's files put in the install folder (setup_files.txt: what an uninstall removes)
    if (src.iso) {
        xiso::Image img;
        std::string err;
        if (!xiso::Open(p.source, img, err))
            return fail(2, err);
        for (const xiso::Entry& e : img.entries)
            placed += e.path + "\n";
        if (!xiso::Extract(img, p.install_dir, {}, on_bytes, prog.cancel, err))
            return fail(prog.cancel ? 4 : 4, err);
    } else if (!fs::equivalent(p.source, p.install_dir, ec)) {
        u64 done = 0;
        for (fs::recursive_directory_iterator it(p.source, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (prog.cancel)
                return fail(4, "Cancelled.");
            const fs::path rel = fs::relative(it->path(), p.source, ec);
            if (IsNotGameData(rel)) {
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
                continue;
            }
            const fs::path dst = p.install_dir / rel;
            placed += Utf8(rel) + "\n";
            if (it->is_directory(ec)) {
                fs::create_directories(dst, ec);
                continue;
            }
            const u64 size = it->file_size(ec);
            if (!(fs::exists(dst, ec) && fs::file_size(dst, ec) == size)) {
                fs::create_directories(dst.parent_path(), ec);
                if (!CopyFileW(it->path().c_str(), dst.c_str(), FALSE))
                    return fail(4, "Could not copy " + Utf8(rel) + " (is the disk full?)");
            }
            done += size;
            on_bytes(done, src.bytes);
        }
    }
    if (!placed.empty())
        util::WriteText(p.install_dir / "setup_files.txt", placed);
    const fs::path xex = p.install_dir / "Default.xex";
    const std::string xex_sha = util::Sha1(xex);
    if (std::find(sdk.xex_sha1.begin(), sdk.xex_sha1.end(), xex_sha) == sdk.xex_sha1.end())
        return fail(2, "The installed Default.xex is not the version this port was made for (the copy is damaged?).");
    note("game files ready: " + util::Gb(src.bytes));
    if (prog.cancel)
        return fail(4, "Cancelled.");

    // ---- 2. translate the game's code to C++
    stage(kTranslate);
    const fs::path gen = p.work_dir / "generated" / "default";
    const fs::path config = p.work_dir / "xmd_config.toml", manifest = p.work_dir / "xmd_manifest.toml";
    const std::string config_text = util::ReadText(p.sdk_dir / "codegen" / "xmd_config.toml");
    const std::string stamp = xex_sha + "\n" + sdk.version + "\n" + std::to_string(std::hash<std::string>{}(config_text)) + "\n";
    const fs::path stamp_file = p.work_dir / "translated.ok";
    if (util::ReadText(stamp_file) == stamp && fs::exists(gen / "xmd_pch.h", ec) && fs::exists(gen / "xmd_init.cpp", ec)) {
        note("already translated");
    } else {
        fs::remove(stamp_file, ec);
        util::WriteText(config, config_text);
        util::WriteText(manifest, "[project]\nname = \"xmd\"\nsdk_version = \"0.10.0\"\ngame_root = \"" + Fwd(p.install_dir) +
                                      "\"\n\n[entrypoint]\nfile_path = \"" + Fwd(xex) +
                                      "\"\nout_directory_path = \"generated/default\"\nincludes = [\"xmd_config.toml\"]\n");
        // rexglue.exe needs Microsoft's C++ runtime: beside its working folder, in case this PC has none installed
        copy_crt(p.work_dir);
        // (about two minutes with nothing to show: the bar creeps towards the end of the step meanwhile)
        std::atomic<bool> ticking{true};
        std::thread tick([&] {
            const auto start = std::chrono::steady_clock::now();
            while (ticking) {
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                prog.fraction = float(std::min(0.95, s / 150.0));
                Sleep(250);
            }
        });
        const int rc = Run(Quote(p.sdk_dir / "bin" / "rexglue.exe") + L" codegen " + Quote(manifest), nullptr, log,
                           BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel, nullptr, p.work_dir);
        ticking = false;
        tick.join();
        if (rc != 0)
            return fail(4, prog.cancel ? "Cancelled." : Format("Translating the game failed (code %d); see %s", rc, Utf8(log).c_str()));
        util::WriteText(stamp_file, stamp);
    }
    std::vector<fs::path> sources;
    for (auto& e : fs::directory_iterator(gen, ec))
        if (e.path().extension() == ".cpp")
            sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());
    if (sources.empty())
        return fail(4, "Translating the game produced no code.");

    // ---- 3. compile it
    stage(kCompile);
    const fs::path obj = p.work_dir / "obj";
    fs::create_directories(obj, ec);
    const fs::path inc = p.sdk_dir / "include";
    // The flags the port's own objects were built with (port\CMakeLists.txt, generated\rexglue.cmake): the guest's
    // SEH scopes need asynchronous exceptions, line tables give crash reports the recompiled function's name.
    const std::string flags =
        "-march=x86-64-v2 -O3 -DNDEBUG -std=gnu++23 -D_DLL -D_MT -Xclang --dependent-lib=msvcrt -msse4.1 "
        "-fasync-exceptions -gline-tables-only -Wno-everything "
        "-DNOMINMAX -DREX_HAS_D3D11=1 -DREX_HAS_D3D12=1 -DREX_HAS_VULKAN=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL "
        "-DWIN32_LEAN_AND_MEAN -I" + QF(gen) + " -I" + QF(inc / "rexglue") + " -I" + QF(inc / "simde") + " -I" +
        QF(inc / "simde" / "simde") + " -I" + QF(inc / "fmt") + " -I" + QF(inc / "spdlog");
    const fs::path pch_hxx = obj / "xmd_pch.hxx", pch_cxx = obj / "xmd_pch.hxx.cxx", pch = obj / "xmd_pch.hxx.pch";
    const u64 sdk_time = util::MTime(p.sdk_dir / "sdk.ini");
    const fs::path flags_file = obj / "flags.txt";
    const std::string flags_id = flags + "\n" + tc.clang_version + "\n" + tc.version + "\n";
    if (util::ReadText(flags_file) != flags_id) { // another compiler or release: everything again
        for (auto& e : fs::directory_iterator(obj, ec))
            fs::remove(e.path(), ec);
        util::WriteText(flags_file, flags_id);
    }
    auto run_clang = [&](const std::string& args, const fs::path& out_log) {
        const fs::path rsp = fs::path(out_log).replace_extension(".rsp");
        util::WriteText(rsp, args);
        return Run(Quote(tc.clang) + L" @" + Quote(rsp), &env, out_log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel);
    };
    if (!(fs::exists(pch, ec) && util::MTime(pch) > util::MTime(gen / "xmd_pch.h") && util::MTime(pch) > sdk_time)) {
        note("compiling the shared header");
        util::WriteText(pch_hxx, "#pragma clang system_header\n#ifdef __cplusplus\n#include \"" + Fwd(gen / "xmd_pch.h") + "\"\n#endif\n");
        util::WriteText(pch_cxx, "/* the shared header of the translated code */\n");
        const fs::path l = obj / "xmd_pch.log";
        fs::remove(l, ec);
        const int rc = run_clang(flags + " -Xclang -emit-pch -Xclang -include -Xclang " + QF(pch_hxx) + " -x c++-header " +
                                     QF(pch_cxx) + " -o " + QF(pch), l);
        if (rc != 0) {
            fs::remove(pch, ec);
            return fail(4, prog.cancel ? "Cancelled." : Format("Compiling failed (code %d); see %s", rc, Utf8(l).c_str()));
        }
    }
    const u64 pch_time = util::MTime(pch);
    prog.total = int(sources.size());
    note(Format("compiling %d parts, %d at a time", int(sources.size()), jobs));
    // largest first, so no long part is left to the end
    std::vector<size_t> order(sources.size());
    std::vector<u64> size(sources.size());
    u64 bytes_all = 0;
    for (size_t i = 0; i < sources.size(); i++) {
        order[i] = i;
        size[i] = fs::file_size(sources[i], ec);
        bytes_all += size[i];
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return size[a] > size[b]; });
    std::atomic<size_t> next{0};
    std::atomic<u64> bytes_compiled{0};
    std::atomic<bool> broken{false};
    std::string broken_why;
    std::mutex broken_mutex;
    auto worker = [&] {
        for (;;) {
            const size_t k = next++;
            if (k >= order.size() || broken || prog.cancel)
                return;
            const size_t i = order[k];
            const fs::path& s = sources[i];
            fs::path o = obj / s.filename();
            o += ".obj";
            if (!(fs::exists(o, ec) && fs::file_size(o, ec) > 0 && util::MTime(o) > util::MTime(s) && util::MTime(o) > pch_time)) {
                fs::path l = obj / s.filename();
                l += ".log";
                fs::remove(l, ec);
                const int rc = run_clang(flags + " -Xclang -include-pch -Xclang " + QF(pch) + " -Xclang -include -Xclang " +
                                             QF(pch_hxx) + " -c " + QF(s) + " -o " + QF(o), l);
                if (rc != 0) {
                    fs::remove(o, ec);
                    if (!prog.cancel) {
                        std::lock_guard lk(broken_mutex);
                        broken_why = Format("Compiling %s failed (code %d); see %s", Utf8(s.filename()).c_str(), rc, Utf8(l).c_str());
                        broken = true;
                    }
                    return;
                }
                fs::remove(l, ec);
                fs::remove(fs::path(l).replace_extension(".rsp"), ec);
            }
            bytes_compiled += size[i];
            const int d = ++prog.done;
            prog.fraction = bytes_all ? float(double(bytes_compiled) / double(bytes_all)) : 1.0f;
            note(Format("compiled %s (%d/%d)", Utf8(s.filename()).c_str(), d, int(sources.size())));
        }
    };
    {
        std::vector<std::thread> pool;
        for (int k = 0; k < std::max(1, jobs); k++)
            pool.emplace_back(worker);
        for (auto& t : pool)
            t.join();
    }
    if (prog.cancel)
        return fail(4, "Cancelled (finished parts are kept; building again continues).");
    if (broken)
        return fail(4, broken_why);

    // ---- 4. link xmd.exe
    stage(kLink);
    const fs::path out_exe = p.work_dir / "xmd.exe";
    {
        std::string r = "-fuse-ld=lld -march=x86-64-v2 -O3 -D_DLL -D_MT -Xclang --dependent-lib=msvcrt -Xlinker /subsystem:windows "
                        "-Wl,/DELAYLOAD:d3d12.dll -Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF -Wl,/PDBALTPATH:xmd.pdb\n";
        for (auto& s : sources) {
            fs::path o = obj / s.filename();
            o += ".obj";
            r += QF(o) + "\n";
        }
        std::vector<fs::path> extra;
        for (const char* d : {"obj", "lib"})
            for (auto& e : fs::directory_iterator(p.sdk_dir / d, ec)) {
                const std::string ext = util::Lower(Utf8(e.path().extension()));
                if (ext == ".obj" || ext == ".lib" || ext == ".res")
                    extra.push_back(e.path());
            }
        std::sort(extra.begin(), extra.end());
        // every object before any library: two of the port's objects define functions rexruntime.lib also offers,
        // and the linker takes the library's when it meets that first (a duplicate symbol)
        for (auto& e : extra)
            if (util::Lower(Utf8(e.extension())) != ".lib")
                r += QF(e) + "\n";
        for (auto& e : extra)
            if (util::Lower(Utf8(e.extension())) == ".lib")
                r += QF(e) + "\n";
        // rexruntime.lib and the static libraries reference each other: listed twice instead of a link group
        for (auto& e : extra)
            if (util::Lower(Utf8(e.extension())) == ".lib")
                r += QF(e) + "\n";
        for (const char* l : {"winmm", "dbghelp", "xaudio2", "ole32", "d3d12", "d3d11", "dxgi", "dxguid", "delayimp", "kernel32",
                              "user32", "gdi32", "imm32", "oleaut32", "version", "uuid", "advapi32", "setupapi", "shell32", "hid",
                              "dinput8", "winspool", "comdlg32", "oldnames"})
            r += std::string("-l") + l + "\n";
        r += "-o " + QF(out_exe) + "\n";
        const fs::path l = p.work_dir / "link.log";
        fs::remove(l, ec);
        prog.fraction = 0.1f;
        if (const int rc = run_clang(r, l); rc != 0)
            return fail(4, prog.cancel ? "Cancelled." : Format("Putting the game together failed (code %d); see %s", rc, Utf8(l).c_str()));
    }
    prog.fraction = 0.8f;
    auto put = [&](const fs::path& from, const fs::path& name) {
        const fs::path to = p.install_dir / name;
        if (CopyFileW(from.c_str(), to.c_str(), FALSE))
            return true;
        note("could not write " + Utf8(to));
        return false;
    };
    if (!put(out_exe, "xmd.exe"))
        return fail(4, "Could not write xmd.exe into " + Utf8(p.install_dir) + " (is the game running?)");
    put(p.work_dir / "xmd.pdb", "xmd.pdb"); // (crash reports name the game's function with it)
    fs::remove(out_exe, ec);
    fs::remove(p.work_dir / "xmd.pdb", ec);

    // what the game needs beside it
    for (auto& e : fs::directory_iterator(p.sdk_dir / "bin", ec))
        if (e.is_regular_file(ec) && util::Lower(Utf8(e.path().extension())) == ".dll" && !put(e.path(), e.path().filename()))
            return fail(4, "Could not write " + Utf8(e.path().filename()) + " into the game folder.");
    copy_crt(p.install_dir);
    fs::create_directories(p.install_dir / "UserData", ec);
    prog.fraction = 1.0f;

    // ---- 5. the native renderer's shaders, converted from the game's own (the release's sdk\shadertools: the
    // port's converter scripts, run by the Python packed with them). Not fatal: without them the game draws with
    // its compatibility renderer, and a repair tries again.
    stage(kShaders);
    {
        std::lock_guard lk(prog.mutex);
        prog.warning.clear();
    }
    const fs::path tools = p.sdk_dir / "shadertools";
    const fs::path shader_work = p.work_dir / "shaders";
    const fs::path shader_out = p.install_dir / "native_shaders";
    const fs::path shader_stamp = p.work_dir / "shaders.ok";
    const std::string shader_id = xex_sha + "\n" + sdk.version + "\n";
    auto shaders_failed = [&](const std::string& why) {
        note("shaders: " + why);
        std::lock_guard lk(prog.mutex);
        prog.warning = "The native renderer's shaders could not be prepared (" + why +
                       "). The game still runs, with its compatibility renderer; Update or repair tries again.";
    };
    if (!p.shaders) {
        note("shader preparation skipped");
    } else if (!fs::exists(tools / "prepare_shaders.py", ec) || !fs::exists(tools / "python" / "python.exe", ec)) {
        shaders_failed("this setup has no shader tools");
    } else if (util::ReadText(shader_stamp) == shader_id && fs::exists(shader_out / "shaders.dxil.pak", ec)) {
        note("shaders already prepared");
    } else {
        fs::remove(shader_stamp, ec);
        if (util::ReadText(shader_work / "converted_by.txt") != sdk.version) { // another release's converter: again
            fs::remove_all(shader_work / "shaders_native", ec);
            util::WriteText(shader_work / "converted_by.txt", sdk.version);
        }
        std::string why = RunShaderTool(p, jobs, prog, env, 0.0f, 0.86f);
        if (why.empty() && !prog.cancel) {
            // The shaders the game compiles itself as it starts (see SavedShadersDir): one short start of the game,
            // silent and off every screen, collects them; then they are converted and packed with the rest.
            note("starting the game once, silent and out of sight, to collect the shaders it makes itself");
            // (the game's start can fail by itself: seen here as an assert three seconds in, several times in a row,
            // then not at all - so up to three tries)
            int saved = 0;
            for (int attempt = 1; attempt <= 3 && saved == 0 && !prog.cancel; attempt++) {
                saved = CollectStartupShaders(p, prog, env);
                note(Format("the game saved %d shaders (start %d)", saved, attempt));
            }
            if (saved > 0 && !prog.cancel)
                why = RunShaderTool(p, jobs, prog, env, 0.93f, 1.0f);
            else if (saved == 0)
                note("none saved: the launcher converts them before a later start instead");
        }
        if (prog.cancel)
            return fail(4, "Cancelled (finished parts are kept; building again continues).");
        if (!why.empty()) {
            shaders_failed(why);
        } else {
            util::WriteText(shader_stamp, shader_id);
            note("shaders prepared");
        }
    }
    prog.fraction = 1.0f;
    const double mins = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60.0;
    note(Format("Done: built in %.0f minutes", mins));
    prog.ok = true;
    prog.running = false;
    prog.finished = true;
    return 0;
}

} // namespace builder
