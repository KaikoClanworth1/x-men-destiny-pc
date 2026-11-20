#include "install_ops.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace setup {

namespace {

constexpr const char* kSetupIni = "setup.ini";
constexpr const char* kFilesList = "setup_files.txt"; // the game's files setup put in the install folder

fs::path KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)))
        out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring UninstallKey() {
    // (test runs: another key, so a real install's entry is never touched)
    return std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\XMenDestinyPC") + (util::Sandbox() ? L"_test" : L"");
}

} // namespace

fs::path SdkDir() {
    if (const std::wstring e = util::Env(L"XMD_SETUP_SDK"); !e.empty())
        return e;
    return util::SetupDir() / "sdk";
}

fs::path DefaultInstallDir() {
    // The drive with the most room among the fixed ones, in a Games folder.
    fs::path best = "C:\\";
    u64 best_free = 0;
    const DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) {
        if (!(drives & (1u << d)))
            continue;
        const std::wstring root = std::wstring(1, wchar_t(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER avail{};
        if (GetDiskFreeSpaceExW(root.c_str(), &avail, nullptr, nullptr) && avail.QuadPart > best_free)
            best = root, best_free = avail.QuadPart;
    }
    return best / "Games" / kGameName;
}

fs::path DefaultBuildDir() { return util::LocalDataDir() / "build"; }

std::string FolderWarning(const fs::path& dir) {
    const std::string d = util::Lower(util::Utf8(dir)) + "\\";
    if (d.find("\\program files") != std::string::npos || d.find("\\windows\\") != std::string::npos)
        return "Windows does not let programs write here, and the game keeps its saves in its own folder.";
    for (const char* place : {"\\onedrive", "\\documents\\", "\\pictures\\", "\\desktop\\"})
        if (d.find(place) != std::string::npos)
            return "Windows (Controlled folder access) or OneDrive can block or move the game's files here.";
    return {};
}

bool CreateShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workdir, const fs::path& icon,
                    const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workdir.c_str());
    link->SetDescription(description.c_str());
    if (!icon.empty())
        link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = false;
    std::error_code ec;
    fs::create_directories(lnk.parent_path(), ec);
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

fs::path DesktopShortcut() {
    const fs::path dir = util::Sandbox() ? fs::path(util::Sandbox()) / "Desktop" : KnownFolder(FOLDERID_Desktop);
    return dir / (std::wstring(kGameName) + L".lnk");
}

fs::path StartMenuShortcut() {
    const fs::path dir = util::Sandbox() ? fs::path(util::Sandbox()) / "StartMenu" : KnownFolder(FOLDERID_Programs);
    return dir / (std::wstring(kGameName) + L".lnk");
}

bool SaveInstalled(const Installed& in) {
    std::ostringstream f;
    f << "# Written by XMD Setup: what it installed (for repair, update and uninstall).\n";
    f << "version=" << in.version << "\nbuild_dir=" << util::Utf8(in.build_dir) << "\nsource=" << util::Utf8(in.source)
      << "\npackage=" << util::Utf8(in.package) << "\ndesktop=" << in.desktop << "\nstart_menu=" << in.start_menu
      << "\nby_launcher=" << in.by_launcher << "\n";
    return util::WriteText(in.dir / kSetupIni, f.str());
}

std::optional<Installed> LoadInstalled(const fs::path& dir) {
    std::ifstream f(dir / kSetupIni);
    if (!f)
        return std::nullopt;
    Installed in;
    in.dir = dir;
    for (std::string line; std::getline(f, line);) {
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "build_dir") in.build_dir = util::Wide(v);
        else if (k == "source") in.source = util::Wide(v);
        else if (k == "package") in.package = util::Wide(v);
        else if (k == "version") in.version = v;
        else if (k == "desktop") in.desktop = v == "1";
        else if (k == "start_menu") in.start_menu = v == "1";
        else if (k == "by_launcher") in.by_launcher = v == "1";
    }
    return in;
}

std::optional<Installed> FindInstalled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, UninstallKey().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;
    wchar_t buf[MAX_PATH * 2]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    const bool ok = RegQueryValueExW(key, L"InstallLocation", nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok)
        return std::nullopt;
    const fs::path dir = buf;
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return std::nullopt;
    if (auto in = LoadInstalled(dir))
        return in;
    Installed in; // the entry without setup.ini: still uninstallable
    in.dir = dir;
    return in;
}

bool RegisterUninstall(const Installed& in, u64 bytes) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    auto str = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
    };
    auto dword = [&](const wchar_t* name, DWORD v) { RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), 4); };
    const std::wstring launcher = (in.dir / kLauncherExe).wstring();
    str(L"DisplayName", L"X-Men Destiny (PC port)");
    str(L"DisplayIcon", launcher);
    str(L"DisplayVersion", util::Wide(in.version));
    str(L"Publisher", L"X-Men Destiny PC port (built on this PC from your own game)");
    str(L"InstallLocation", in.dir.wstring());
    str(L"UninstallString", L"\"" + launcher + L"\" --uninstall");
    str(L"ModifyPath", L"\"" + launcher + L"\"");
    str(L"URLInfoAbout", util::Wide(std::string("https://github.com/") + kRepo));
    dword(L"EstimatedSize", DWORD(bytes >> 10));
    dword(L"NoRepair", 1);
    RegCloseKey(key);
    return true;
}

void UnregisterUninstall() { RegDeleteKeyW(HKEY_CURRENT_USER, UninstallKey().c_str()); }

bool FinishInstall(Installed& in, std::string& error) {
    std::error_code ec;
    const fs::path launcher = in.dir / kLauncherExe;
    if (!fs::equivalent(util::ExePath(), launcher, ec)) {
        if (!CopyFileW(util::ExePath().c_str(), launcher.c_str(), FALSE)) {
            error = "Could not write the launcher into " + util::Utf8(in.dir) + ".";
            return false;
        }
    }
    const std::wstring about = L"X-Men Destiny, the PC port built from your own game";
    for (const auto& [want, lnk] : {std::pair{in.desktop, DesktopShortcut()}, std::pair{in.start_menu, StartMenuShortcut()}}) {
        if (want)
            CreateShortcut(lnk, launcher, in.dir, launcher, about);
        else
            fs::remove(lnk, ec);
    }
    SaveInstalled(in);
    RegisterUninstall(in, util::FolderBytes(in.dir));
    return true;
}

std::vector<std::string> Uninstall(const Installed& in, bool keep_saves) {
    std::vector<std::string> left;
    std::error_code ec;
    auto remove_all = [&](const fs::path& p) {
        if (!fs::exists(p, ec))
            return;
        fs::remove_all(p, ec);
        if (ec)
            left.push_back(util::Utf8(p) + ": " + ec.message());
        ec.clear();
    };
    for (const fs::path& lnk : {DesktopShortcut(), StartMenuShortcut()})
        remove_all(lnk);
    UnregisterUninstall();
    // The build files (always setup's own folder: it is made for the build) and downloaded packages.
    if (!in.build_dir.empty() && fs::exists(in.build_dir / "build.log", ec))
        remove_all(in.build_dir);
    remove_all(util::LocalDataDir() / "packages");
    remove_all(util::LocalDataDir() / "tools");
    if (in.dir.empty() || !(fs::exists(in.dir / "xmd.exe", ec) || fs::exists(in.dir / kSetupIni, ec)))
        return left;
    // The game's files setup put there (listed when they were installed). Anything else the player added stays.
    {
        std::ifstream list(in.dir / kFilesList);
        std::vector<fs::path> dirs;
        for (std::string line; std::getline(list, line);) {
            while (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line.find("..") != std::string::npos || line.find(':') != std::string::npos)
                continue;
            const fs::path p = in.dir / fs::path(util::Wide(line));
            if (fs::is_directory(p, ec))
                dirs.push_back(p);
            else
                fs::remove(p, ec);
        }
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
        for (const fs::path& d : dirs)
            fs::remove(d, ec); // (only when empty)
        ec.clear();
    }
    // What setup built and the game wrote.
    for (const char* p : {"xmd.exe", "xmd.pdb", "rexruntime.dll", "rexgpu-xenos.dll", "rexffmpeg.dll", "rexmspack.dll",
                          "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll",
                          kSetupIni, kFilesList, "native_shaders", "logs", "cmdbuf", "crashes"})
        remove_all(in.dir / p);
    for (const char* p : {"cache", "logs", "crashes"})
        remove_all(in.dir / "UserData" / p);
    if (!keep_saves) {
        remove_all(in.dir / "UserData");
        remove_all(in.dir / "xmd.toml");
    } else {
        fs::remove(in.dir / "UserData", ec); // (only when empty)
        ec.clear();
    }
    return left;
}

void RemoveSelfLater(const fs::path& install_dir) {
    // A hidden cmd waits for this process to end, deletes the launcher and the folder if nothing is left in it.
    const fs::path launcher = install_dir / kLauncherExe;
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + launcher.wstring() + L"\" & rmdir \"" +
                       install_dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace setup
