#include "toolchain.h"

#include <cstdio>
#include <fstream>

#include <windows.h>
#include <shellapi.h>

namespace toolchain {

namespace {

using util::Format;
using util::Run;

fs::path ProgramFiles(const wchar_t* var, const wchar_t* fallback) {
    const std::wstring v = util::Env(var);
    return v.empty() ? fs::path(fallback) : fs::path(v);
}

fs::path VsInstallerDir() {
    return ProgramFiles(L"ProgramFiles(x86)", L"C:\\Program Files (x86)") / "Microsoft Visual Studio" / "Installer";
}

// One Visual Studio / Build Tools install, as vswhere lists it.
struct VsInstance {
    fs::path path;
    std::string product; // Microsoft.VisualStudio.Product.BuildTools, ...Community, ...
    int major = 0;       // installationVersion: 18 = Visual Studio 2026
    bool cpp = false;    // has the C++ x64 tools
};

std::vector<VsInstance> ListVs(const wchar_t* extra) {
    std::string out;
    Run(L"\"" + (VsInstallerDir() / "vswhere.exe").wstring() + L"\" -all -prerelease -products * " + extra +
            L" -format text -utf8",
        nullptr, {}, NORMAL_PRIORITY_CLASS, nullptr, &out);
    std::vector<VsInstance> v;
    size_t at = 0;
    while (at < out.size()) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos)
            end = out.size();
        std::string line = out.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t c = line.find(": ");
        if (c == std::string::npos)
            continue;
        const std::string key = line.substr(0, c), val = line.substr(c + 2);
        if (key == "instanceId")
            v.emplace_back();
        else if (v.empty())
            continue;
        else if (key == "installationPath")
            v.back().path = fs::path(util::Wide(val));
        else if (key == "productId")
            v.back().product = val;
        else if (key == "installationVersion")
            v.back().major = std::atoi(val.c_str());
    }
    return v;
}

constexpr int kVsMajor = 18; // Visual Studio 2026: the version whose C++ library the prebuilt libraries come from

// XMD_SETUP_CLANG (a path), else Visual Studio's own clang, else LLVM's install.
fs::path FindClang(const fs::path& vs_path) {
    std::vector<fs::path> c;
    if (const std::wstring e = util::Env(L"XMD_SETUP_CLANG"); !e.empty())
        c.push_back(e);
    if (!vs_path.empty())
        c.push_back(vs_path / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "clang++.exe");
    c.push_back(ProgramFiles(L"ProgramW6432", L"C:\\Program Files") / "LLVM" / "bin" / "clang++.exe");
    std::error_code ec;
    for (auto& p : c)
        if (fs::exists(p, ec) && fs::exists(p.parent_path() / "lld-link.exe", ec))
            return p;
    return {};
}

std::string ClangVersion(const fs::path& clang) {
    std::string out;
    Run(util::Quote(clang) + L" --version", nullptr, {}, NORMAL_PRIORITY_CLASS, nullptr, &out);
    const size_t at = out.find("version ");
    if (at == std::string::npos)
        return {};
    size_t end = at + 8;
    while (end < out.size() && (isdigit(static_cast<unsigned char>(out[end])) || out[end] == '.'))
        end++;
    return out.substr(at + 8, end - at - 8);
}

// Starts a program the way Explorer would (so Windows asks for admin rights when it needs them), and waits.
int RunElevatable(const fs::path& exe, const std::wstring& args, std::string& error) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        error = e == ERROR_CANCELLED ? "Windows' admin prompt was declined: the C++ tools need it to install."
                                     : Format("Microsoft's installer could not start (error %lu).", e);
        return -1;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

} // namespace

Toolchain Find(int min_msvc_minor) {
    Toolchain t;
    if (const std::wstring e = util::Env(L"XMD_SETUP_MIN_MSVC"); !e.empty()) // (developer test of the "too old" path)
        min_msvc_minor = _wtoi(e.c_str());
    if (!fs::exists(VsInstallerDir() / "vswhere.exe")) {
        t.problem = "Microsoft C++ Build Tools are not installed.";
        t.fix = Toolchain::Fix::Install;
        return t;
    }
    // Every Visual Studio / Build Tools (also preview channels), not only the newest install.
    auto all = ListVs(L"");
    for (const auto& c : ListVs(L"-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"))
        for (auto& a : all)
            if (a.path == c.path)
                a.cpp = true;
    // a Visual Studio 2026 without C++ (Build Tools first) that the C++ workload can be added to
    const VsInstance* bare = nullptr;
    for (const auto& a : all)
        if (!a.cpp && a.major >= kVsMajor && (!bare || a.product.find("BuildTools") != std::string::npos))
            bare = &a;

    Toolchain best;
    int best_minor = -1;
    const VsInstance* best_vs = nullptr;
    for (const auto& vs : all) {
        if (!vs.cpp)
            continue;
        Toolchain c;
        c.vcvars = vs.path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
        std::ifstream ver(vs.path / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt");
        std::getline(ver, c.version); // 14.51.36231
        while (!c.version.empty() && (c.version.back() == '\r' || c.version.back() == ' '))
            c.version.pop_back();
        int major = 0, minor = 0;
        std::sscanf(c.version.c_str(), "%d.%d", &major, &minor);
        util::Log(Format("C++ tools: %s (%s, %s)", util::Utf8(vs.path).c_str(), vs.product.c_str(), c.version.c_str()));
        if (!fs::exists(c.vcvars) || major != 14 || minor <= best_minor)
            continue;
        best = c, best_minor = minor, best_vs = &vs;
    }
    auto add_cpp_or_install = [&] {
        if (bare)
            t.fix = Toolchain::Fix::AddCpp, t.fix_path = bare->path, t.fix_product = bare->product;
        else
            t.fix = Toolchain::Fix::Install;
    };
    if (best_minor < 0) {
        t.problem = all.empty() ? "Microsoft C++ Build Tools are not installed."
                    : bare      ? "Visual Studio is installed without its C++ tools (\"Desktop development with C++\")."
                                : "No Visual Studio 2026 with its C++ tools is installed.";
        add_cpp_or_install();
        return t;
    }
    t = best;
    t.fix_path = best_vs->path, t.fix_product = best_vs->product;
    if (best_minor < min_msvc_minor) {
        // (the prebuilt libraries come from tools 14.<min>: an older C++ library can't link them)
        t.problem = Format("The C++ tools are too old (%s; 14.%d or newer is needed).", t.version.c_str(), min_msvc_minor);
        if (best_vs->major >= kVsMajor)
            t.fix = Toolchain::Fix::Update;
        else
            add_cpp_or_install();
        return t;
    }
    t.ok = true;
    t.clang = FindClang(best_vs->path);
    if (t.clang.empty()) {
        t.problem = "The clang compiler is not installed (Visual Studio's \"C++ Clang Compiler for Windows\").";
        t.fix = best_vs->major >= kVsMajor ? Toolchain::Fix::AddClang : Toolchain::Fix::Install;
    } else {
        t.clang_version = ClangVersion(t.clang);
        util::Log(Format("clang: %s (%s)", util::Utf8(t.clang).c_str(), t.clang_version.c_str()));
    }
    return t;
}

const char* FixLabel(const Toolchain& t) {
    switch (t.fix) {
    case Toolchain::Fix::Install: return "Install for me";
    case Toolchain::Fix::Update: return "Update for me";
    case Toolchain::Fix::AddCpp: return "Add C++ for me";
    case Toolchain::Fix::AddClang: return "Add clang for me";
    default: return "";
    }
}

bool FixTools(const Toolchain& t, const std::function<void(const std::string&)>& note, std::string& error) {
    // the C++ workload (Build Tools: VCTools; Visual Studio: NativeDesktop) with its recommended parts (the Windows
    // SDK), and clang: the compiler the game's code is built with
    const std::wstring clang_parts = L" --add Microsoft.VisualStudio.Component.VC.Llvm.Clang"
                                     L" --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset";
    auto workload = [](const std::string& product) {
        return std::wstring(product.find("BuildTools") != std::string::npos ? L" --add Microsoft.VisualStudio.Workload.VCTools"
                                                                            : L" --add Microsoft.VisualStudio.Workload.NativeDesktop") +
               L" --includeRecommended";
    };
    const std::wstring quiet = L" --passive --norestart"; // Microsoft's progress window, no questions, no restart
    const fs::path installer = VsInstallerDir() / "setup.exe";
    fs::path exe;
    std::wstring args;
    switch (t.fix) {
    case Toolchain::Fix::Install: {
        // Microsoft's Build Tools 2026 installer (a small download that fetches the rest)
        std::error_code ec;
        const fs::path dir = util::LocalDataDir() / "tools";
        fs::create_directories(dir, ec);
        exe = dir / "vs_buildtools.exe";
        fs::remove(exe, ec);
        note("Downloading Microsoft's Build Tools installer");
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        const int dl = Run(L"\"" + (fs::path(sys) / "curl.exe").wstring() + L"\" -L -f -s -o \"" + exe.wstring() +
                               L"\" https://aka.ms/vs/18/stable/vs_buildtools.exe",
                           nullptr, dir / "download.log", NORMAL_PRIORITY_CLASS);
        if (dl != 0 || !fs::exists(exe, ec) || fs::file_size(exe, ec) < 100000) {
            error = "Could not download Microsoft's Build Tools installer. Check the internet connection, or install "
                    "\"Build Tools for Visual Studio 2026\" from visualstudio.microsoft.com.";
            return false;
        }
        args = L"--wait" + quiet + workload("BuildTools") + clang_parts;
        note("Installing Microsoft's C++ Build Tools (about 6 GB)");
        break;
    }
    case Toolchain::Fix::Update:
        exe = installer;
        args = L"update --installPath \"" + t.fix_path.wstring() + L"\"" + quiet;
        note("Updating Visual Studio's C++ tools");
        break;
    case Toolchain::Fix::AddCpp:
        exe = installer;
        args = L"modify --installPath \"" + t.fix_path.wstring() + L"\"" + workload(t.fix_product) + clang_parts + quiet;
        note("Adding the C++ tools to Visual Studio");
        break;
    case Toolchain::Fix::AddClang:
        exe = installer;
        args = L"modify --installPath \"" + t.fix_path.wstring() + L"\"" + clang_parts + quiet;
        note("Adding the clang compiler to Visual Studio");
        break;
    default: return true;
    }
    if (!fs::exists(exe)) {
        error = "Microsoft's Visual Studio Installer was not found.";
        return false;
    }
    util::Log("C++ tools fix: \"" + util::Utf8(exe) + "\" " + util::Utf8(args));
    const int code = RunElevatable(exe, args, error);
    util::Log(Format("C++ tools fix: exit code %d", code));
    if (code == -1)
        return false;
    if (code != 0 && code != 3010) { // (3010: done, a restart is recommended)
        error = code == 1602 || code == 5007 ? "Microsoft's installer was cancelled."
                                             : Format("Microsoft's installer stopped (code %d). Run it again, or open "
                                                      "the Visual Studio Installer to finish.", code);
        return false;
    }
    return true;
}

bool CaptureEnvironment(const fs::path& vcvars, std::vector<wchar_t>& block) {
    std::string out;
    const std::wstring cmd = L"cmd.exe /u /c \"\"" + vcvars.wstring() + L"\" >nul 2>&1 && set\"";
    if (Run(cmd, nullptr, {}, NORMAL_PRIORITY_CLASS, nullptr, &out) != 0 || out.size() < 2)
        return false;
    const std::wstring text(reinterpret_cast<const wchar_t*>(out.data()), out.size() / 2);
    block.clear();
    bool tools = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring line = text.substr(at, end - at);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        if (line.find(L'=') != std::wstring::npos && line[0] != L'=') {
            if (_wcsnicmp(line.c_str(), L"VCToolsInstallDir=", 18) == 0)
                tools = true;
            block.insert(block.end(), line.begin(), line.end());
            block.push_back(0);
        }
        at = end + 1;
    }
    block.push_back(0);
    return tools;
}

std::vector<wchar_t> WithPathFirst(const std::vector<wchar_t>& block, const fs::path& dir) {
    std::vector<wchar_t> out;
    bool had = false;
    for (const wchar_t* e = block.data(); *e; e += wcslen(e) + 1) {
        std::wstring line = e;
        if (_wcsnicmp(e, L"PATH=", 5) == 0) {
            line = L"PATH=" + dir.wstring() + L";" + line.substr(5);
            had = true;
        }
        out.insert(out.end(), line.begin(), line.end());
        out.push_back(0);
    }
    if (!had) {
        const std::wstring line = L"PATH=" + dir.wstring();
        out.insert(out.end(), line.begin(), line.end());
        out.push_back(0);
    }
    out.push_back(0);
    return out;
}

std::vector<wchar_t> WithVar(const std::vector<wchar_t>& block, const std::wstring& name, const std::wstring& value) {
    std::vector<wchar_t> out;
    for (const wchar_t* e = block.data(); *e; e += wcslen(e) + 1) {
        if (_wcsnicmp(e, name.c_str(), name.size()) == 0 && e[name.size()] == L'=')
            continue;
        out.insert(out.end(), e, e + wcslen(e) + 1);
    }
    const std::wstring line = name + L"=" + value;
    out.insert(out.end(), line.begin(), line.end());
    out.push_back(0);
    out.push_back(0);
    return out;
}

std::wstring EnvValue(const std::vector<wchar_t>& block, const wchar_t* name) {
    const size_t n = wcslen(name);
    for (const wchar_t* e = block.data(); *e; e += wcslen(e) + 1)
        if (_wcsnicmp(e, name, n) == 0 && e[n] == L'=')
            return e + n + 1;
    return {};
}

} // namespace toolchain
