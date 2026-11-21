// X-Men: Destiny - ReXGlue recompiled PC port: application hooks.

#include "generated/default/xmd_init.h"

#include "xmd_app.h"
#include "script_input.h"
#include "controls.h"
#include "pad_icons.h"
#include "crash_report.h"
#include "capture.h"
#include "debug_commands.h"
#include "perf_overlay.h"
#include "mod_menu.h"
#include "history.h"
#include "pc_menu.h"
#include "problem_log.h"
#if XMD_NATIVE
#include "native/native_renderer.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>

#include <rex/audio/sdl/sdl_audio_system.h>

#include "xaudio2_audio.h"
#endif

REXCVAR_DEFINE_INT32(frame_rate_limit, 0, "GPU",
                     "Frames a second at most (0: not limited - as fast as the PC draws; 30 is the console's)");
REXCVAR_DEFINE_STRING(audio_backend, "xaudio2", "Audio", "Audio output: xaudio2 (default) or sdl")
    .allowed({"xaudio2", "sdl"});

namespace {

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::filesystem::path g_problem_file, g_user_data, g_config_file;

}  // namespace

void XmdApp::OnConfigurePaths(rex::PathConfig& paths) {
#if defined(_WIN32)
  // 1 ms timer resolution: without it Windows rounds every Sleep up to the
  // 15.6 ms default tick.
  timeBeginPeriod(1);
#endif

  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  if (paths.game_data_root.empty()) {
    paths.game_data_root = exe_dir;
  }
  paths.user_data_root = exe_dir / "UserData";
  // Tests: XMD_USER_DATA=<folder> keeps their saves away from the player's.
  if (std::string dir = Env("XMD_USER_DATA"); !dir.empty()) {
    paths.user_data_root = dir;
  }
  paths.cache_root = paths.user_data_root / "cache";
  xmd::InstallCrashReporter(paths.user_data_root / "crashes");
  // The session's problems, apart from the full log (problem_log.h).
  g_user_data = paths.user_data_root;
  g_problem_file = paths.user_data_root / "logs" / "problems.log";
  if (std::string file = Env("XMD_PROBLEM_FILE"); !file.empty()) g_problem_file = file;
  // Tests: XMD_CONFIG=<file> keeps settings changes away from the player's.
  if (std::string config = Env("XMD_CONFIG"); !config.empty()) {
    paths.config_path = config;
  }
  g_config_file = paths.config_path;  // (the Graphics menu writes its settings there: pc_menu.h)
  xmd::pcmenu::RevertUnconfirmed(g_config_file);  // (a setting the last session did not survive: back, before the file is read)
}

void XmdApp::OnPreSetup(rex::RuntimeConfig& config) {
#if defined(_WIN32)
  // Which CPUs the game may run on: XMD_AFFINITY=<hex mask> (the test scripts
  // set FF00, so a test never competes with what the user is playing).
  if (std::string mask = Env("XMD_AFFINITY"); !mask.empty()) {
    const DWORD_PTR bits = static_cast<DWORD_PTR>(std::strtoull(mask.c_str(), nullptr, 16));
    if (bits) SetProcessAffinityMask(GetCurrentProcess(), bits);
  }
#endif
  xmd::problems::Install(g_problem_file);  // (now: logging is up, no game thread yet)
  if (!Env("XMD_AUDIO_MUTE").empty()) {
    rex::cvar::SetFlagByName("audio_mute", "true");
  }
  xmd::controls::Configure(g_config_file);  // (keyboard and mouse on, with this game's layout: controls.h)
  // Automated tests: the only controller is one driven by a command file.
  if (std::string file = Env("XMD_INPUT_FILE"); !file.empty()) {
    xmd::InstallScriptLogSink();  // (now: logging is up, no game thread yet)
    config.input_factory = [file](bool) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = std::make_unique<rex::input::InputSystem>(nullptr);
      auto driver = std::make_unique<xmd::ScriptInputDriver>(file);
      driver->Setup();
      input->AddDriver(std::move(driver));
      // (the script's "key" and "mouse" lines: the keyboard-and-mouse driver in its test mode - it reads no real device)
      rex::input::mnk::SetTestMode(true);
      input->AddDriver(std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0));
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
      return input;
    };
  }
  // A played session recorded for the tests (tools\record_play.ps1): XMD_RECORD_FILE=<file> gets what the
  // player does on the controller from the log line XMD_RECORD_FROM on (script_input.h, "pad" lines).
  else if (std::string file = Env("XMD_RECORD_FILE"); !file.empty()) {
    xmd::InstallScriptLogSink();
    const std::string from = Env("XMD_RECORD_FROM");
    config.input_factory = [file, from](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = rex::input::CreateDefaultInputSystem(tool_mode);
      xmd::InstallPadRecorder(input.get(), file, from);
      return input;
    };
  }
#if defined(_WIN32)
  // Sound goes out through XAudio2 (setting audio_backend = "sdl": the SDK's
  // SDL output); SDL too when XAudio2 cannot start (no audio device, ...).
  if (REXCVAR_GET(audio_backend) != "sdl") {
    config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher)
        -> std::unique_ptr<rex::system::IAudioSystem> {
      if (xmd::XAudio2AudioSystem::IsAvailable()) {
        return xmd::XAudio2AudioSystem::Create(dispatcher);
      }
      REXLOG_WARN("XAudio2 unavailable, using SDL audio");
      return rex::audio::sdl::SDLAudioSystem::Create(dispatcher);
    };
  }
#endif
  // The frame rate. The game shows a frame every second tick of the console's display clock (30 a second at
  // 60 Hz) and takes its own time from the real clock, so the clock's rate is the frame rate's limit:
  // frame_rate_limit 0 runs it at 1000 Hz (as fast as the PC draws, up to 500) and shows frames without
  // waiting for the screen; a number runs it at twice that. An explicit guest_vblank_hz is left alone.
  if (rex::cvar::Query<double>("guest_vblank_hz") <= 0.0) {
    const int32_t limit = REXCVAR_GET(frame_rate_limit);
    rex::cvar::SetFlagByName("guest_vblank_hz", std::to_string(limit > 0 ? 2 * std::clamp(limit, 15, 500) : 1000));
    if (limit <= 0 && rex::cvar::GetFlagSource("vsync") == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName("vsync", "false");
    REXLOG_INFO("frame rate: {}", limit > 0 ? fmt::format("limited to {} a second", limit) : std::string("not limited"));
  }
  if (config.gpu_plugin.empty()) {
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    config.gpu_plugin = "xenos";
  }
}

void XmdApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // The window's name and icon (the runtime's default: the project's short name and its build stamp).
  if (rex::ui::Window* w = window()) w->SetTitle("X-Men Destiny");
#if defined(_WIN32)
  if (const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))) {
    struct Find {
      static BOOL CALLBACK Each(HWND window, LPARAM icon) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && GetWindow(window, GW_OWNER) == nullptr) {
          SendMessageW(window, WM_SETICON, ICON_BIG, icon);
          SendMessageW(window, WM_SETICON, ICON_SMALL, icon);
        }
        return TRUE;
      }
    };
    EnumWindows(&Find::Each, reinterpret_cast<LPARAM>(icon));
  }
#endif
  xmd::InstallPadIcons();                        // (button prompts for the device in use: pad_icons.h)
  xmd::InstallPerfOverlay(drawer);               // (F2: FPS, CPU and GPU time a frame)
  xmd::InstallCapture(g_user_data, g_problem_file);  // (F4: screenshot, draws, log, where the player is)
  xmd::modmenu::Install(drawer, g_user_data);        // (F1: the mod menu - mod_menu.h)
}

void XmdApp::AttachNativeRenderer() {
#if XMD_NATIVE
  // The native renderer (setting native_renderer = "main"; "off": the SDK's
  // emulated Xbox GPU draws). It reads the game's vertex and texture data
  // where the game put it.
  // ("auto", the default, and "main": xmd::native decides - and hands over to the emulated renderer, with the
  // reason in the window's title, when it can't start or stops)
  if (const std::string mode = rex::cvar::Query<std::string>("native_renderer"); mode == "main" || mode == "auto") {
    xmd::native::SetFailureNotice([this](const std::string& reason) {
      app_context().CallInUIThread([this, reason] {
        if (rex::ui::Window* w = window())
          w->SetTitle("X-Men Destiny - emulated renderer (the native renderer can't draw: " + reason + ")");
      });
    });
    xmd::native::Attach(runtime()->memory());
    if (rex::ui::Window* w = window()) {
      xmd::native::SetWindowSizeSource([w] {
        return std::pair<uint32_t, uint32_t>(w->GetActualPhysicalWidth(), w->GetActualPhysicalHeight());
      });
    }
  }
#endif
}

void XmdApp::OnPostLoadXexImage() {
  xmd::history::Install(runtime()->virtual_membase());  // (history.h: what an F4 capture tells of the last 5 minutes)
  AttachNativeRenderer();
  // (Exit on the main menu, Graphics under Options: pc_menu.h; Exit closes the window as its X button does)
  xmd::pcmenu::Install(
      runtime()->memory()->SystemHeapAlloc(xmd::pcmenu::GuestBlockSize()),
      [this] {
        app_context().CallInUIThread([this] {
          if (rex::ui::Window* w = window()) w->RequestClose();
        });
      },
      [this](std::function<void()> f) { app_context().CallInUIThread(std::move(f)); }, g_config_file);
  xmd::controls::Install(runtime()->virtual_membase(), [this] { return InteractiveOverlayOpen(); },
                         static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  // Tests: XMD_DEBUG_FILE=<file>, lines run as Lua in the game's script state (debug_commands.cpp).
  // (without one: still there for the port's own questions to the scripts - an F4 capture's "where")
  xmd::debug::Install(Env("XMD_DEBUG_FILE"), runtime()->memory()->SystemHeapAlloc(xmd::debug::ScratchSize()));
  // The mod menu holds the game's controller back while it is open (mod_menu.h).
  xmd::modmenu::AttachGame(static_cast<rex::input::InputSystem*>(runtime()->input_system()),
                           runtime()->memory()->SystemHeapAlloc(xmd::modmenu::ScratchSize()));
  // Developer aid: XMD_DUMP_IMAGE=<file> writes the loaded (decrypted,
  // decompressed) executable image for analysis tools (port/tools/).
  if (std::string dump = Env("XMD_DUMP_IMAGE"); !dump.empty()) {
    const uint8_t* image = runtime()->virtual_membase() + REX_IMAGE_BASE;
    std::ofstream(dump, std::ios::binary)
        .write(reinterpret_cast<const char*>(image), REX_IMAGE_SIZE);
  }
}



