// The launcher's updater: asks GitHub for the newest release of the port, downloads its package (the same zip players
// download by hand), unpacks it under %LOCALAPPDATA%\XMenDestinyPC\packages\<version> and starts its XMDSetup.exe with
// --update <install folder>, which rebuilds the installed game with the new version from the game files already
// installed. The package holds no part of the game.
//
// Developer tests: XMD_UPDATE_API=<local .json file or url> replaces GitHub's answer, XMD_UPDATE_CURRENT=<version>
// replaces this build's version.
#pragma once
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "util.h"

namespace updater {

struct Release {
    std::string version; // "v0.3.5"
    std::string url;     // the Windows package (.zip)
    u64 size = 0;
};

// Numbers of a version ("v0.3.4", "0.3"): true when `a` is newer than `b`. False when either has none.
bool Newer(const std::string& a, const std::string& b);

// GitHub's latest release (blocking, about a second; a 20 s limit). Empty with `error` set when it can't tell.
std::optional<Release> Latest(std::string& error);

// The whole update, on its own thread: check, then (Start) download, unpack, run the new setup.
struct Updater {
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Unpacking, Ready, Failed };
    std::atomic<State> state{State::Idle};
    std::atomic<float> progress{0};
    Release release;    // (once Available)
    std::string error;  // (mutex) why it failed
    fs::path setup_exe; // (once Ready) the new package's XMDSetup.exe
    std::mutex mutex;

    // Looks for a newer release than `current` (this build's version). Nothing happens without a repository.
    void Check(const std::string& current);
    // Downloads and unpacks the release found (State::Available). `keep`: a package folder still in use, never
    // removed; older packages are.
    void Start(const fs::path& keep);
    // Runs the new setup (State::Ready) to update the install in `install_dir`; true when it started.
    bool Run(const fs::path& install_dir);
    ~Updater();

private:
    std::thread thread;
    void Join();
};

} // namespace updater
