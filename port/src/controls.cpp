// X-Men: Destiny - keyboard and mouse play, mouse look (see controls.h).
//
// What the game does with the controller (found 2026-11-20/21, controls chat):
//   sub_825FDBF0(pad, dt)   every frame: XamInputGetState(user [pad+8]) -> buttons (sub_825FDF08), sticks * 1/32768,
//                           triggers * 1/255, then
//   sub_825FD6D0(pad, lx, rx, ry, ly, dt)   per axis the dead zone ([pad+548], rescaled) and the Options menu's
//                           invert flags ([pad+552] ly, [+556] lx, [+560] ry, [+564] rx: non-zero = * -1), then the
//                           results: left stick x [pad+48] y [pad+52] length [pad+56], right stick x [pad+248]
//                           y [pad+252] length [pad+256], one-sided copies at [pad+232..244].
//   sub_826BE5E8(camera)    copies the active row of the CameraSettings table (408 bytes a row) into the camera:
//                           yaw speed [cam+4264], yaw range [+4268], yaw linear end point [+4272], pitch speed
//                           [+4276], range [+4280], linear end [+4284], constant-speed threshold [+4300], spin
//                           speed multiplier [+4304], spin acceleration [+4308], spin delay [+4312].

#include "controls.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "generated/default/xmd_init.h"
#include "mod_menu.h"

REXCVAR_DEFINE_DOUBLE(mouse_sensitivity, 1.0, "Input", "Mouse look speed (1 = the default)").range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(mouse_invert_y, false, "Input", "Mouse look: up and down swapped");

namespace xmd::controls {

namespace {

uint8_t* g_base = nullptr;
std::function<bool()> g_overlay_open;
std::atomic<uint32_t> g_camera{0}, g_pad{0};

float LoadF(uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, g_base + address, 4);
  return std::bit_cast<float>(std::byteswap(raw));
}
void StoreF(uint32_t address, float value) {
  const uint32_t raw = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(g_base + address, &raw, 4);
}
uint32_t LoadU(uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, g_base + address, 4);
  return std::byteswap(raw);
}

// The layout: a PC action game's. `value` is the runtime's key list (alternatives joined with commas).
struct Default {
  const char* setting;
  const char* value;
};
const Default kLayout[] = {
    {"keybind_lstick_up", "W"},          {"keybind_lstick_down", "S"},
    {"keybind_lstick_left", "A"},        {"keybind_lstick_right", "D"},
    {"keybind_x", "LMB"},                // light attack
    {"keybind_y", "RMB"},                // heavy attack
    {"keybind_a", "Space,Return"},       // jump / accept
    {"keybind_b", "Shift,Backspace"},    // dodge / back
    {"keybind_right_shoulder", "Q,MMB"}, // block
    {"keybind_left_shoulder", "R"},      // X-Mode
    {"keybind_right_trigger", "E"},      // mutant powers (held with an attack key)
    {"keybind_left_trigger", "F"},       // focus camera
    {"keybind_lstick_press", "C"},       {"keybind_rstick_press", "V"},
    {"keybind_rstick_up", ""},           {"keybind_rstick_down", ""},
    {"keybind_rstick_left", ""},         {"keybind_rstick_right", ""},
    {"keybind_dpad_up", "Up"},           {"keybind_dpad_down", "Down"},
    {"keybind_dpad_left", "Left"},       {"keybind_dpad_right", "Right"},
    {"keybind_start", "Escape"},         // pause
    {"keybind_back", "Tab"},             // map
    {"keybind_guide", ""},
};

// Camera probe (debug lines "cam scan" / "cam diff"): which of the camera's values move.
std::mutex g_scan_mutex;
std::vector<float> g_scan;
constexpr uint32_t kScanBytes = 4800;

std::atomic<int> g_trace{0};
std::atomic<uint32_t> g_camera_vtable{0};

// Mouse look. The camera's orientation is the quaternion at [cam+592] (x y z w = turn about the up axis by the
// yaw, then about the side axis by the pitch; the game's own stick code turns it and the camera then places
// itself around the player from it; [cam+576] is a copy, [cam+832] / [cam+836] the pitch / yaw it shows).
// The mouse turns that quaternion itself: so many radians a count, whatever the frame rate - none of the stick's
// dead zone, response curve or spin-up. The pitch stops where the stick's does (measured in level 1: 28 degrees
// up, 55 down). While the game does not run its camera (pause and other menus: [cam+96], the camera's own frame
// time, stands still) the mouse does nothing.
constexpr float kRadiansPerCount = 0.0012f;
constexpr float kPitchUp = -0.485f, kPitchDown = 0.955f;
uint32_t g_camera_tick = 0;
int64_t g_camera_tick_ms = 0;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void MouseLook(uint32_t pad) {
  using namespace rex::input::mnk;
  SetMouseLookActive(!(g_overlay_open && g_overlay_open()) && !xmd::modmenu::IsOpen());
  float dx = 0, dy = 0;
  TakeMouseDelta(&dx, &dy);
  const uint32_t cam = g_camera.load();
  if (!cam || LoadU(cam) != g_camera_vtable.load()) return;  // (no camera, or its memory is something else now)
  const int64_t now = NowMs();
  if (const uint32_t tick = LoadU(cam + 96); tick != g_camera_tick) {
    g_camera_tick = tick;
    g_camera_tick_ms = now;
  }
  if (!IsMouseCaptured() || now - g_camera_tick_ms > 100 || (dx == 0 && dy == 0)) return;
  const float x = LoadF(cam + 592), y = LoadF(cam + 596), z = LoadF(cam + 600), w = LoadF(cam + 604);
  const float length = x * x + y * y + z * z + w * w;
  if (!(length > 0.96f && length < 1.04f)) return;
  // (where it looks: the forward axis turned by the quaternion)
  const float fx = 1.0f - 2.0f * (y * y + z * z), fy = 2.0f * (x * y + w * z), fz = 2.0f * (x * z - w * y);
  float yaw = std::atan2(fy, fx), pitch = -std::asin(std::clamp(fz, -1.0f, 1.0f));
  const float step = kRadiansPerCount * static_cast<float>(REXCVAR_GET(mouse_sensitivity));
  // (the Options menu's Invert Camera X / Y hold for the mouse too)
  yaw += dx * step * (LoadU(pad + 564) ? -1.0f : 1.0f);
  // (the pitch stays inside the stick's limits - or where a camera of the game's own had put it beyond them)
  const float old_pitch = pitch;
  pitch += dy * step * ((LoadU(pad + 560) != 0) != REXCVAR_GET(mouse_invert_y) ? -1.0f : 1.0f);
  pitch = std::clamp(pitch, std::min(old_pitch, kPitchUp), std::max(old_pitch, kPitchDown));
  const float sz = std::sin(yaw * 0.5f), cz = std::cos(yaw * 0.5f), sp = std::sin(pitch * 0.5f),
              cp = std::cos(pitch * 0.5f);
  StoreF(cam + 592, -sz * sp);
  StoreF(cam + 596, cz * sp);
  StoreF(cam + 600, sz * cp);
  StoreF(cam + 604, cz * cp);
}

}  // namespace

void Configure() {
  using rex::cvar::GetFlagSource;
  using rex::cvar::SetFlagByName;
  using rex::cvar::Source;
  if (GetFlagSource("mnk_mode") == Source::kDefault) SetFlagByName("mnk_mode", "true");
  if (GetFlagSource("mnk_mouse") == Source::kDefault) SetFlagByName("mnk_mouse", "true");
  for (const Default& d : kLayout) {
    if (GetFlagSource(d.setting) == Source::kDefault) SetFlagByName(d.setting, d.value);
  }
  rex::input::mnk::SetMouseDeltaSink(true);
}

void Install(uint8_t* membase, std::function<bool()> overlay_open) {
  g_base = membase;
  g_overlay_open = std::move(overlay_open);
}

bool Command(const std::string& verb, const std::string& rest) {
  if (verb != "cam" && verb != "controls") return false;
  std::istringstream in(rest);
  std::string what;
  in >> what;
  const uint32_t cam = g_camera.load(), pad = g_pad.load();
  if (verb == "controls") {
    if (what == "trace") {
      int frames = 60;
      in >> frames;
      g_trace = frames;
    } else if (pad) {
      REXLOG_INFO("controls: pad {:08X} left {:.3f} {:.3f} right {:.3f} {:.3f} dead zone {:.3f} invert {} {} {} {} "
                  "captured {}",
                  pad, LoadF(pad + 48), LoadF(pad + 52), LoadF(pad + 248), LoadF(pad + 252), LoadF(pad + 548),
                  LoadU(pad + 552), LoadU(pad + 556), LoadU(pad + 560), LoadU(pad + 564),
                  rex::input::mnk::IsMouseCaptured());
    } else {
      REXLOG_INFO("controls: no pad read yet");
    }
    return true;
  }
  if (!cam) {
    REXLOG_INFO("cam: no camera seen yet");
    return true;
  }
  if (what == "scan") {
    std::lock_guard lock(g_scan_mutex);
    g_scan.resize(kScanBytes / 4);
    for (uint32_t i = 0; i < kScanBytes / 4; ++i) g_scan[i] = LoadF(cam + i * 4);
    REXLOG_INFO("cam: scan taken of {:08X}", cam);
  } else if (what == "diff") {
    std::lock_guard lock(g_scan_mutex);
    std::string text;
    int count = 0;
    for (uint32_t i = 0; i < g_scan.size(); ++i) {
      const float now = LoadF(cam + i * 4), was = g_scan[i];
      if (now == was || !(std::fabs(now) < 1e7f) || !(std::fabs(was) < 1e7f)) continue;
      if (std::fabs(now) < 1e-6f && std::fabs(was) < 1e-6f) continue;
      text += fmt::format(" +{}: {:.4f}->{:.4f}", i * 4, was, now);
      if (++count % 8 == 0) {
        REXLOG_INFO("cam: diff{}", text);
        text.clear();
      }
      g_scan[i] = now;
    }
    REXLOG_INFO("cam: diff{} ({} values changed)", text, count);
  } else if (what == "peek") {
    uint32_t offset = 0, count = 8;
    in >> offset >> count;
    std::string text;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
      text += fmt::format(" +{}: {:.4f} ({:08X})", offset + i * 4, LoadF(cam + offset + i * 4),
                          LoadU(cam + offset + i * 4));
    }
    REXLOG_INFO("cam: peek{}", text);
  } else if (what == "poke") {
    uint32_t offset = 0;
    float value = 0;
    in >> offset >> value;
    StoreF(cam + offset, value);
    REXLOG_INFO("cam: poke +{} = {}", offset, value);
  } else {
    REXLOG_INFO("cam: camera {:08X}, settings row {:08X}", cam,
                LoadU(cam + 4328) + (LoadU(cam + 4360) * 2 + LoadU(cam + 4356)) * 408);
  }
  return true;
}

}  // namespace xmd::controls

// The game's stick function, once a frame: the moment the mouse turns the camera (MouseLook above).
REX_EXTERN(__imp__sub_825FD6D0);
REX_HOOK_RAW(sub_825FD6D0) {
  using namespace xmd::controls;
  const uint32_t pad = ctx.r3.u32;
  const float dt = static_cast<float>(ctx.f5.f64);  // (the frame's time, for the trace)
  __imp__sub_825FD6D0(ctx, base);
  if (!g_base) return;
  if (LoadU(pad + 8) != 0) return;  // (the first player's controller only)
  if (g_pad.exchange(pad) != pad) REXLOG_INFO("controls: the game's controller object is {:08X}", pad);
  MouseLook(pad);
  if (g_trace > 0) {
    --g_trace;
    REXLOG_INFO("controls: trace dt {:.4f} right {:.3f} {:.3f} left {:.3f} {:.3f}", dt, LoadF(pad + 248),
                LoadF(pad + 252), LoadF(pad + 48), LoadF(pad + 52));
  }
}

// The camera takes a row of its settings table: remembers which object the camera is.
REX_EXTERN(__imp__sub_826BE5E8);
REX_HOOK_RAW(sub_826BE5E8) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_826BE5E8(ctx, base);
  xmd::controls::g_camera_vtable = xmd::controls::LoadU(camera);
  if (xmd::controls::g_camera.exchange(camera) != camera) {
    REXLOG_INFO("controls: the camera object is {:08X}", camera);
  }
}
