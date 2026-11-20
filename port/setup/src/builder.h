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
    bool shaders = true;  // prepare the native renderer's shaders (developer tests of the rest turn it off)
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
    std::atomic<int> shaders_done{0}, shaders_total{0}; // shaders converted
    std::mutex mutex;
    std::string error;
    std::string warning; // the build worked, but the player should know (the shaders could not be prepared)
    std::vector<std::string> log; // last lines
    void Log(std::string s);
    std::string GetError();
    std::string GetWarning();
    std::vector<std::string> Tail(size_t n);
    float Overall() const; // 0..1 over all steps
};

// Runs every step (blocking; the window calls it on a worker thread). `jobs` compilers run at once.
// Exit classes for the command line: 0 ok, 2 game input, 3 tools, 4 anything else.
int BuildGame(const Paths& paths, int jobs, Progress& progress);
// The game compiles a few shaders of its own while it runs (its program holds a shader compiler and their source):
// they are in none of its files, so the running game saves each one it has no converted shader for into this folder
// (<install>\UserData\cache\shaders_new\*.xsc). AddSavedShaders converts them and packs them with the rest (blocking;
// seconds for a few shaders); the launcher does it before it starts the game.
fs::path SavedShadersDir(const fs::path& install_dir);
int SavedShaderCount(const fs::path& install_dir);
bool AddSavedShaders(const Paths& paths, int jobs, Progress& progress, std::string& why);
// Compiler jobs that fit this PC: cores - 1, at most one per 2 GB of RAM.
int DefaultJobs();
constexpr u64 kBuildFilesBytes = 2000ull << 20;     // generated C++, compiled parts, converted shaders
constexpr u64 kProgramBytes = 500ull << 20;         // xmd.exe, its symbols, the DLLs, the shader packs

} // namespace builder
