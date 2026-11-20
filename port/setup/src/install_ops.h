// XMD Setup: what it does outside its window and the build - folders, the shortcuts, the Windows "Apps" entry, the
// record of what it installed (setup.ini) and the uninstall.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "util.h"

namespace setup {

constexpr const wchar_t* kGameName = L"X-Men Destiny";
// The setup's own exe, installed next to xmd.exe: it opens as the launcher there (shortcuts, Apps entry).
constexpr const wchar_t* kLauncherExe = L"X-Men Destiny Launcher.exe";
// GitHub "owner/name" of the port's repository (releases, issues); set by the build (port\setup\CMakeLists.txt,
// XMD_REPO). Empty: no update check, no links.
#ifndef XMD_REPO
#define XMD_REPO ""
#endif
constexpr const char* kRepo = XMD_REPO;
constexpr const char* kCredit = "By KaikoClanworth";

fs::path SdkDir();            // the release's sdk\ folder (XMD_SETUP_SDK=<folder> for developers)
fs::path DefaultInstallDir(); // the fixed drive with the most room: <drive>\Games\X-Men Destiny
fs::path DefaultBuildDir();   // LocalDataDir()\build
// Folders Windows blocks programs writing in or moves files out of (the game keeps its saves beside its exe):
// the reason as a sentence, or empty when the folder is fine.
std::string FolderWarning(const fs::path& dir);

bool CreateShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workdir, const fs::path& icon,
                    const std::wstring& description);
fs::path DesktopShortcut();
fs::path StartMenuShortcut();

// What setup installed and where (setup.ini in the install folder and the Apps entry), so re-running it can repair,
// update or uninstall exactly what it made.
struct Installed {
    fs::path dir;        // install folder
    fs::path build_dir;  // build files
    fs::path source;     // the player's .iso or folder it was installed from
    fs::path package;    // the setup package it was built with (its sdk\)
    std::string version; // the release it was built with
    bool desktop = false, start_menu = false;
    bool by_launcher = false; // installed with --prepare (a launcher owns it: no shortcuts, no Apps entry, no self-update)
};
std::optional<Installed> FindInstalled(); // from the Apps entry
bool SaveInstalled(const Installed& in);  // setup.ini
std::optional<Installed> LoadInstalled(const fs::path& dir);
bool RegisterUninstall(const Installed& in, u64 bytes);
void UnregisterUninstall();
// Copies this exe into the install folder as the launcher and makes the chosen shortcuts and the Apps entry.
bool FinishInstall(Installed& in, std::string& error);

// Removes what setup made: the game's files it put in the install folder, the built game, caches and logs, the build
// files, the shortcuts and the Apps entry. Saves, settings and captures (UserData\, xmd.toml) stay unless keep_saves
// is false. Returns what could not be removed (empty: all gone).
std::vector<std::string> Uninstall(const Installed& in, bool keep_saves);
// Deletes the launcher exe (this running program) and the then-empty install folder once this process has ended.
void RemoveSelfLater(const fs::path& install_dir);

} // namespace setup
