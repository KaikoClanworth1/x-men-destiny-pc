// Builds the game on the player's PC from their own copy: the disc's files are put in the install folder, the game's
// code (Default.xex) is translated to C++ by the release's rexglue.exe, compiled with the player's clang and linked
// with the port's prebuilt objects and libraries (the release's sdk\ folder). The release itself holds none of the game.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "util.h"

namespace builder {

// ---- the player's copy of the game
enum class Verdict { Supported, WrongVersion, NotThisGame, Unreadable };
struct GameSource {
    fs::path path;    // an .iso, or a folder holding Default.xex
    bool iso = false;
    u64 bytes = 0;    // what it installs (the disc's files)
    Verdict verdict = Verdict::Unreadable;
    std::string detail; // why, for the tooltip
    std::string Describe() const; // "Disc image · 8.1 GB"
    const char* Badge() const;    // "Supported version", ...
};
// Looks at a disc image or folder: is it this game, in the version the port was made for?
GameSource Probe(const fs::path& path, const fs::path& sdk_dir);
// Disc images and extracted folders in the usual places (the setup's folder, Downloads, Desktop, Documents, Games /
// ROMs / Isos folders of every fixed drive), by name.
std::vector<fs::path> FindGameSources(std::atomic<bool>& cancel);

// ---- the release's sdk\ folder
struct Sdk {
    fs::path dir;
    std::string version;   // "0.1.0"
    int min_msvc_minor = 0; // 51 = tools 14.51
    std::vector<std::string> xex_sha1; // the versions of Default.xex the port supports
    std::string problem;   // not empty: the release is incomplete
};
Sdk LoadSdk(const fs::path& dir);

struct Paths {
    fs::path sdk_dir;     // the release's sdk\ (include\, lib\, obj\, bin\, codegen\, tools\rexglue.exe)
    fs::path install_dir; // the game folder: the disc's files, xmd.exe, the DLLs, UserData\ (never touched)
    fs::path work_dir;    // generated C++, compiled parts, logs
    fs::path source;      // the player's .iso or folder
};

enum Step { kRead, kTranslate, kCompile, kLink, kShaders, kStepCount };
extern const char* const kStepNames[kStepCount];
extern const float kStepWeights[kStepCount];

struct Progress {
    std::atomic<bool> running{false}, finished{false}, ok{false}, cancel{false};
    std::atomic<int> step{0};
    std::atomic<float> fraction{0}; // of the current step
    std::atomic<int> done{0}, total{0}; // compiled parts
    std::atomic<u64> bytes_done{0}, bytes_total{0}; // game files read
    std::mutex mutex;
    std::string error;
    std::vector<std::string> log; // last lines
    void Log(std::string s);
    std::string GetError();
    std::vector<std::string> Tail(size_t n);
    float Overall() const; // 0..1 over all steps
};

// Runs every step (blocking; the window calls it on a worker thread). `jobs` compilers run at once.
// Exit classes for the command line: 0 ok, 2 game input, 3 tools, 4 anything else.
int BuildGame(const Paths& paths, int jobs, Progress& progress);
// Compiler jobs that fit this PC: cores - 1, at most one per 2 GB of RAM.
int DefaultJobs();
constexpr u64 kBuildFilesBytes = 1300ull << 20;     // generated C++, compiled parts
constexpr u64 kProgramBytes = 250ull << 20;         // xmd.exe, its symbols, the DLLs

} // namespace builder
