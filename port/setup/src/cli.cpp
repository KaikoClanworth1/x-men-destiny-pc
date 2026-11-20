// XMD Setup without a window: `XMDSetup.exe --prepare --game=<iso or folder> --install=<folder>` (launchers run this).
// stdout: "progress <0..1> <step>", "note <text>", "error <text>". Exit codes: 0 ok, 2 the game input is missing or
// not this game, 3 the C++ tools are missing and could not be installed, 4 anything else failed.
#include "cli.h"

#include <chrono>
#include <thread>

#include <windows.h>
#include <shellapi.h>

#include "builder.h"
#include "install_ops.h"
#include "toolchain.h"

namespace cli {

namespace {

void Out(const std::string& line) {
    const std::string s = line + "\n";
    DWORD n = 0;
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE)
        WriteFile(h, s.data(), DWORD(s.size()), &n, nullptr);
}

} // namespace

std::string Args::Get(const std::string& name) const {
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i] == "--" + name && i + 1 < list.size() && list[i + 1].rfind("--", 0) != 0)
            return list[i + 1];
        if (list[i].rfind("--" + name + "=", 0) == 0)
            return list[i].substr(name.size() + 3);
    }
    return {};
}
bool Args::Has(const std::string& name) const {
    for (const std::string& a : list)
        if (a == "--" + name || a.rfind("--" + name + "=", 0) == 0)
            return true;
    return false;
}

Args Parse() {
    Args a;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++)
        a.list.push_back(util::Utf8(std::wstring(argv[i])));
    LocalFree(argv);
    return a;
}

int Prepare(const Args& args) {
    const std::string game = args.Get("game");
    builder::Paths p;
    p.sdk_dir = setup::SdkDir();
    p.install_dir = args.Get("install").empty() ? util::SetupDir() / "game" : fs::absolute(fs::path(util::Wide(args.Get("install"))));
    p.work_dir = args.Get("work").empty() ? setup::DefaultBuildDir() : fs::absolute(fs::path(util::Wide(args.Get("work"))));
    p.source = fs::path(util::Wide(game));
    std::error_code ec;
    fs::create_directories(p.install_dir / "UserData" / "logs", ec);
    util::SetLogFile(p.install_dir / "UserData" / "logs" / "setup.log");
    util::Log("---- prepare: " + util::Utf8(std::wstring(GetCommandLineW())));
    if (game.empty() || !fs::exists(p.source, ec)) {
        // an update of an installed game needs no input again: its files are in the install folder already
        if (game.empty() && fs::exists(p.install_dir / "Default.xex", ec)) {
            p.source = p.install_dir;
        } else {
            Out("error The game was not given (--game=<your .iso, or the folder with Default.xex>).");
            return 2;
        }
    }
    const builder::Sdk sdk = builder::LoadSdk(p.sdk_dir);
    if (!sdk.problem.empty()) {
        Out("error " + sdk.problem);
        return 4;
    }
    toolchain::Toolchain tc = toolchain::Find(sdk.min_msvc_minor);
    if (!tc.Ready()) {
        if (args.Has("no-tools-install")) {
            Out("error " + tc.problem);
            return 3;
        }
        Out("note " + tc.problem + " Starting Microsoft's installer.");
        std::string error;
        if (!toolchain::FixTools(tc, [](const std::string& s) { Out("note " + s); }, error)) {
            Out("error " + error);
            return 3;
        }
        tc = toolchain::Find(sdk.min_msvc_minor);
        if (!tc.Ready()) {
            Out("error " + (tc.problem.empty() ? std::string("The C++ tools are still missing.") : tc.problem));
            return 3;
        }
    }
    int jobs = std::atoi(args.Get("jobs").c_str());
    if (jobs <= 0)
        jobs = builder::DefaultJobs();
    builder::Progress prog;
    int code = 4;
    std::thread worker([&] { code = builder::BuildGame(p, jobs, prog); });
    float last = -1;
    int last_step = -1;
    while (!prog.finished) {
        const float f = prog.Overall();
        if (f - last >= 0.002f || prog.step != last_step) {
            last = f;
            last_step = prog.step;
            Out(util::Format("progress %.3f %s", f, builder::kStepNames[last_step]));
        }
        Sleep(250);
    }
    worker.join();
    if (code != 0) {
        Out("error " + prog.GetError());
        return code;
    }
    // what a repair, update or uninstall needs to know (no shortcuts or Apps entry: the launcher owns the install)
    setup::Installed in;
    in.dir = p.install_dir;
    in.build_dir = p.work_dir;
    in.source = p.source;
    in.version = sdk.version;
    in.by_launcher = true;
    setup::SaveInstalled(in);
    Out("progress 1.000 Ready to play");
    return 0;
}

} // namespace cli
