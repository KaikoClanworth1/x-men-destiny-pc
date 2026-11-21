// X-Men: Destiny - the port's own entries in the game's menus (see pc_menu.h).
//
// How the game's menus work (all found by reading the movies with tools\gfx_dis.py and the recompiled code
// with tools\fn.py / tools\xref.py):
//
// A menu is a Flash movie (Scaleform 3, ActionScript 2) plus a C++ object of the game that owns it. The game
// object keeps the Scaleform movie at [+428] and a queue of call arguments at [+464] (16 bytes each: kind at
// +4 - 2 boolean, 3 number, 4 text, 5 wide text - value at +8), their count at [+468].
//   sub_82718E68(object, text)        queue a text argument        sub_82718DF8(object, int)   a number
//   sub_82349098(object, path, wide)  queue a wide text, then call the movie's function `path`
//   sub_82718CF0(object, path)        call the movie's function `path` with what is queued
//   sub_82718900(object, path, wide)  set the movie's variable `path` to a text; sub_82718748: to a boolean
// The movie calls the game with ExternalInterface.call(name, ...): sub_8271A058 (r5 = name, r6 = arguments,
// r7 = their count), hooked in lua_debug.cpp, which asks OnFlashCall here first.
//
// The main menu (movie XMDMenuMain, filled by sub_824920F8 / sub_82492348): ClearMainListViewItems, four
// _root.AddMainListViewItem[Disabled](text), _root.mMainListView.UpdateText. The movie answers a press with
// OnSelectMenuOption(index). "Exit" is a fifth item added just before UpdateText; its button exists because
// the movie is changed while it loads (menu_patch.cpp).
//
// Options (movie XMDMenuOptions, list filled by sub_824B2EB0 with AddMainListViewItem x5): the movie plays the
// sub-menu's frame itself and tells the game SelectMenuOption(index). The sixth entry, "Graphics", is the
// port's: index 5 never reaches the game; the movie is sent to the frame "Graphics" that menu_patch.cpp adds,
// whose script and key handlers call PCGraphicsPopulate / PCGraphicsLeft, Right, Press(row) / PCGraphicsClose -
// answered here: the rows' texts, the settings (the runtime's cvars) and their file.
//
// The title screen ("A Start") and why Options would not open: pressing A makes the game play the movie's
// "IISButtonPress" frames, whose last one calls IISCompleted - the game's sub_8248EDE8, which notes that the
// title screen is over ([game + 692] = 0, [game + 696] = 1; game = [0x8366855C]). Meanwhile the game asks for
// the storage device and lists the saves; on the console that takes seconds, here 0.2 s - less than the ten
// frames of the animation. The saves' listener (sub_823ED760) then fills the main menu and sends the movie on
// to "MainMenuIntro" before its last title frame: IISCompleted never came, [game + 692] stayed 1, and the
// Options menu, which leaves at once while that is set (its tick sub_824B2D98 calls its own "exit": a save,
// whose end closes the menu - sub_82509990), closed 8 ms after opening, every time. Here: when the main menu is
// filled while that animation is still under way, the game's IISCompleted handler is run first.
//
// A movie is read by Scaleform from a memory file the game makes (vtable 0x8226BD38: bytes at [+12], size at
// [+16], position at [+20]); sub_827D99E8 starts reading it. That is where a changed copy is swapped in.

#include "pc_menu.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/xmd_init.h"
#include "controls.h"
#include "menu_patch.h"

REX_EXTERN(__imp__sub_82718CF0);
REX_EXTERN(__imp__sub_82718900);
REX_EXTERN(__imp__sub_82718748);
REX_EXTERN(__imp__sub_827D99E8);
REX_EXTERN(__imp__sub_824920F8);
REX_EXTERN(__imp__sub_82492348);
REX_EXTERN(__imp__sub_824B2EB0);
REX_EXTERN(__imp__sub_824B27A8);
REX_EXTERN(__imp__sub_82492AA8);
REX_EXTERN(sub_82718E68);
REX_EXTERN(sub_82718DF8);
REX_EXTERN(sub_8248EDE8);

namespace xmd::pcmenu {

namespace {

constexpr uint32_t kMemoryFileVtable = 0x8226BD38;
// The guest memory block Install is given: texts handed to the game at the start, then a changed movie each.
constexpr uint32_t kTextRoom = 0x1000,  // (texts: slots of 128 bytes)
     kMainMenuRoom = 0x20000, kOptionsRoom = 0x40000;

bool g_trace = false;
uint32_t g_block = 0;
std::function<void()> g_exit;
std::function<void(std::function<void()>)> g_on_ui_thread;
std::filesystem::path g_config;
uint32_t g_options_object = 0;  // (the game's object of the Options menu whose list has "Graphics")
std::mutex g_mutex;
uint32_t g_main_menu_size = 0, g_options_size = 0;  // (the changed movies, once made; 0: not yet, 1: refused)
thread_local int g_filling_main_menu = 0;
uint32_t g_main_menu_object = 0;  // (the game's object of the main menu whose list has "Exit")
bool g_exit_listed = false;       // (the main menu's list is the game's four entries + "Exit")
bool g_confirming_exit = false;   // (the "Exit Game?" page is up)
bool g_title_ending = false;      // (the title screen's "button pressed" frames play; IISCompleted not yet seen)
bool g_difficulty_hidden = false; // (the game hid the Options list's fifth entry, as it does on the main menu)
// The Options legend's X entry ("Select Storage Device") as the game last set it: the Controls page borrows it.
std::string g_legend_x_text;
bool g_legend_x_shown = true, g_legend_x_borrowed = false;

uint32_t Guest32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return _byteswap_ulong(v);
}
void PutGuest32(uint8_t* base, uint32_t address, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + address, &v, 4);
}
double GuestDouble(uint8_t* base, uint32_t address) {
  uint64_t v;
  std::memcpy(&v, base + address, 8);
  v = _byteswap_uint64(v);
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}
std::string GuestString(uint8_t* base, uint32_t address, size_t limit = 200) {
  std::string out;
  if (address < 0x10000) return "(null)";
  const char* p = reinterpret_cast<const char*>(base + address);
  for (size_t i = 0; i < limit && p[i]; ++i) out.push_back(p[i]);
  return out;
}
std::string GuestWideString(uint8_t* base, uint32_t address, size_t limit = 200) {
  std::string out;
  if (address < 0x10000) return "(null)";
  for (size_t i = 0; i < limit; ++i) {
    const uint8_t hi = base[address + i * 2], lo = base[address + i * 2 + 1];
    if (!hi && !lo) break;
    out.push_back(hi ? '?' : char(lo));
  }
  return out;
}

// The arguments of a call between the game and a movie, for the log.
std::string Arguments(uint8_t* base, uint32_t values, uint32_t count) {
  std::string out;
  for (uint32_t i = 0; i < count && i < 8 && values >= 0x10000; ++i) {
    const uint32_t at = values + i * 16, kind = Guest32(base, at + 4) & 0xFF;
    if (i) out += ", ";
    if (kind == 3) {
      out += fmt::format("{}", GuestDouble(base, at + 8));
    } else if (kind == 4) {
      out += "\"" + GuestString(base, Guest32(base, at + 8)) + "\"";
    } else if (kind == 5) {
      out += "L\"" + GuestWideString(base, Guest32(base, at + 8)) + "\"";
    } else if (kind == 2) {
      out += base[at + 8] ? "true" : "false";
    } else {
      out += fmt::format("kind {} {:08X} {:08X}", kind, Guest32(base, at + 8), Guest32(base, at + 12));
    }
  }
  return out;
}

// A text of the port's in guest memory (slots of 128 bytes in the block's first part).
uint32_t Text(uint8_t* base, int slot, const std::string& text) {
  const uint32_t at = g_block + uint32_t(slot) * 128;
  const size_t n = std::min<size_t>(text.size(), 127);
  std::memcpy(base + at, text.data(), n);
  base[at + n] = 0;
  return at;
}

// movie.path(text) / movie.path() - a call of one of the movie's functions, from inside a hook: the game's
// registers are put back afterwards.
void CallMovie(PPCContext& ctx, uint8_t* base, uint32_t object, const char* path, const std::string* text = nullptr,
               const int* number = nullptr) {
  if (!g_block || object < 0x10000) return;
  const PPCContext saved = ctx;
  ctx.r1.u32 -= 512;
  if (text) {
    ctx.r3.u64 = object;
    ctx.r4.u64 = Text(base, 1, *text);
    sub_82718E68(ctx, base);
  }
  if (number) {
    ctx.r3.u64 = object;
    ctx.r4.s64 = *number;
    sub_82718DF8(ctx, base);
  }
  ctx.r3.u64 = object;
  ctx.r4.u64 = Text(base, 0, path);
  __imp__sub_82718CF0(ctx, base);
  ctx = saved;
}

// movie.path = text / = on-off, through the game's own setters (16-bit characters for the text).
void SetMovieText(PPCContext& ctx, uint8_t* base, uint32_t object, const std::string& path, const std::string& text) {
  if (!g_block || object < 0x10000) return;
  const uint32_t wide = g_block + 3 * 128;  // (slots 3-6: 255 characters)
  size_t n = 0;
  for (; n < text.size() && n < 255; ++n) base[wide + n * 2] = 0, base[wide + n * 2 + 1] = uint8_t(text[n]);
  base[wide + n * 2] = base[wide + n * 2 + 1] = 0;
  const PPCContext saved = ctx;
  ctx.r1.u32 -= 512;
  ctx.r3.u64 = object;
  ctx.r4.u64 = Text(base, 0, path);
  ctx.r5.u64 = wide;
  __imp__sub_82718900(ctx, base);
  ctx = saved;
}
void SetMovieFlag(PPCContext& ctx, uint8_t* base, uint32_t object, const std::string& path, bool on) {
  if (!g_block || object < 0x10000) return;
  const PPCContext saved = ctx;
  ctx.r1.u32 -= 512;
  ctx.r3.u64 = object;
  ctx.r4.u64 = Text(base, 0, path);
  ctx.r5.u64 = on ? 1 : 0;
  __imp__sub_82718748(ctx, base);
  ctx = saved;
}

// ---- the Graphics page's settings ----
//
// Each row is one of the runtime's settings (rex::cvar) the native renderer or the window really reads:
//   fullscreen              the window: borderless over the screen / a window (applies at once, on the UI thread)
//   native_scene_height     the scene's lines: 0 the window's own, else 720 ... 2160 (read twice a second)
//   native_aa               supersampling: 0 none, 2-4 times the lines (read twice a second)
//   vsync                   the presenter's swap chain is made with it: next start
//   frame_rate_limit        at start it sets guest_vblank_hz (the game shows a frame every second tick); the
//                           runtime reads guest_vblank_hz at every tick, so setting both applies at once
//   native_shadows          read twice a second
//   native_texture_quality  read twice a second (textures are uploaded again)
//   native_anisotropy       read when a sampler is made, and samplers are kept: next start

struct Choice {
  const char* value;  // (the cvar's)
  const char* text;
};
struct Row {
  const char* label;
  const char* cvar;
  bool tick;  // (an on / off setting: the row's tick box shows it)
  std::vector<Choice> choices;
  const char* note;
};
const std::vector<Row>& Rows() {
  static const std::vector<Row> rows = {
      {"Fullscreen", "fullscreen", true, {{"false", ""}, {"true", ""}}, ""},
      {"Resolution", "native_scene_height", false,
       {{"0", "Native"}, {"720", "1280 x 720"}, {"900", "1600 x 900"}, {"1080", "1920 x 1080"},
        {"1440", "2560 x 1440"}, {"2160", "3840 x 2160"}}, ""},
      {"Antialiasing", "native_aa", false, {{"0", "Off"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}}, ""},
      {"VSync", "vsync", true, {{"false", ""}, {"true", ""}}, " (next start)"},
      {"Frame Rate Limit", "frame_rate_limit", false,
       {{"30", "30"}, {"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"}, {"240", "240"},
        {"0", "Unlimited"}}, ""},
      {"Shadows", "native_shadows", true, {{"false", ""}, {"true", ""}}, ""},
      {"Texture Quality", "native_texture_quality", false, {{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}}, ""},
      {"Texture Filtering", "native_anisotropy", false,
       {{"0", "Trilinear"}, {"2", "Anisotropic 2x"}, {"4", "Anisotropic 4x"}, {"8", "Anisotropic 8x"},
        {"16", "Anisotropic 16x"}}, " (next start)"},
  };
  return rows;
}

bool NextStart(const Row& row) { return row.note[0] != 0; }

// ---- settings on trial ----
//
// A setting chosen on the page is written to the settings file at once, but it is "on trial" until the game has
// run with it: the trial file (<settings file>.trial) holds the value it had before. A row that applies while
// running is kept after kKeepAfter of running with it; a "next start" row after the next start has run that long.
// A session that ends before that without the program leaving in the ordinary way (the GPU hung, a crash, the
// process ended from outside) leaves the entry there, and the next start puts the old value back before the
// settings are read - so the page cannot leave the game with a setting it does not start with.
// A line of the trial file:  <state> <name> = <the old value, as the settings file writes it>
//   live    changed in the session that wrote it      -> put back at the next start
//   next    a "next start" row, not yet tried          -> becomes "trying" at the next start
//   trying  a "next start" row, the session using it   -> put back at the next start
// Presses are also gathered: the setting itself changes kApplyAfter after the last press, so stepping through a
// row's choices is one change for the renderer, not one a press.
constexpr auto kApplyAfter = std::chrono::milliseconds(600);
constexpr auto kKeepAfter = std::chrono::seconds(15), kKeepAfterStart = std::chrono::seconds(60);

struct Trial {
  std::string state, name, old_value;
};
using Clock = std::chrono::steady_clock;
std::mutex g_settings_mutex;
std::condition_variable g_settings_wake;
std::vector<Trial> g_trials;
std::map<std::string, std::string> g_wanted;  // (what the page shows: chosen, not yet applied or for the next start)
Clock::time_point g_apply_at = Clock::time_point::max(), g_keep_at = Clock::time_point::max();
std::vector<std::string> g_start_notes;  // (what RevertUnconfirmed did, for the log once it is up)

std::filesystem::path TrialFile(const std::filesystem::path& config) {
  std::filesystem::path file = config;
  file += ".trial";
  return file;
}
std::vector<Trial> ReadTrials(const std::filesystem::path& config) {
  std::vector<Trial> trials;
  std::ifstream in(TrialFile(config));
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t space = line.find(' '), equals = line.find(" = ");
    if (space == std::string::npos || equals == std::string::npos || equals <= space) continue;
    trials.push_back({line.substr(0, space), line.substr(space + 1, equals - space - 1), line.substr(equals + 3)});
  }
  return trials;
}
void WriteTrials(const std::filesystem::path& config, const std::vector<Trial>& trials) {
  if (config.empty()) return;
  std::error_code ec;
  if (trials.empty()) {
    std::filesystem::remove(TrialFile(config), ec);
    return;
  }
  std::filesystem::create_directories(config.parent_path(), ec);
  std::ofstream out(TrialFile(config), std::ios::trunc);
  for (const Trial& t : trials) out << t.state << " " << t.name << " = " << t.old_value << "\n";
  out.flush();
}

// `name = value` lines into the settings file: only these lines are written, whatever else the file holds stays
// (other settings, the [log.levels] table).
void WriteConfigLines(const std::filesystem::path& config, const std::vector<std::pair<std::string, std::string>>& values) {
  if (config.empty()) return;
  std::vector<std::string> lines;
  {
    std::ifstream in(config);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  size_t tables = lines.size();  // (top-level settings come before the first [table])
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!lines[i].empty() && lines[i][0] == '[') {
      tables = i;
      break;
    }
  }
  for (const auto& [name, value] : values) {
    const std::string line = name + " = " + value;
    bool found = false;
    for (size_t i = 0; i < tables; ++i) {
      const size_t name_end = lines[i].find_first_of(" =\t");
      if (lines[i].compare(0, name_end == std::string::npos ? lines[i].size() : name_end, name) == 0 &&
          lines[i].find('=') != std::string::npos) {
        lines[i] = line;
        found = true;
        break;
      }
    }
    if (!found) lines.insert(lines.begin() + std::ptrdiff_t(tables++), line);
  }
  std::error_code ec;
  std::filesystem::create_directories(config.parent_path(), ec);
  std::ofstream out(config, std::ios::trunc);
  for (const std::string& line : lines) out << line << "\n";
}

// A setting's value as the settings file writes it.
std::string FileValue(const char* cvar, const std::string& value) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(cvar);
  return info && info->type == rex::cvar::FlagType::String ? "\"" + value + "\"" : value;
}

// What the page shows for a setting: the choice made on it, else the setting itself.
std::string Wanted(const Row& row) {
  {
    std::lock_guard lock(g_settings_mutex);
    if (const auto it = g_wanted.find(row.cvar); it != g_wanted.end()) return it->second;
  }
  return rex::cvar::GetFlagByName(row.cvar);
}

// Which of the row's choices the setting has now (a value not listed: the nearest number, else the first).
size_t Current(const Row& row) {
  const std::string now = Wanted(row);
  for (size_t i = 0; i < row.choices.size(); ++i) {
    if (now == row.choices[i].value) return i;
  }
  if (row.tick) return now == "1" || now == "true" ? 1 : 0;
  char* end = nullptr;
  const double value = std::strtod(now.c_str(), &end);
  if (end == now.c_str()) return 0;
  size_t best = 0;
  double best_distance = 1e30;
  for (size_t i = 0; i < row.choices.size(); ++i) {
    const double distance = std::fabs(std::strtod(row.choices[i].value, nullptr) - value);
    if (distance < best_distance) best = i, best_distance = distance;
  }
  return best;
}

std::string RowText(const Row& row) {
  if (row.tick) return std::string(row.label) + row.note;
  return std::string(row.label) + ":  " + row.choices[Current(row)].text + row.note;
}

// The settings into the settings file (as the page shows them). Not rex::cvar::SaveConfig: that writes every setting that
// differs from its default - guest_vblank_hz too, which the port sets itself at start and would then no longer
// follow frame_rate_limit.
void SaveSettings() {
  if (g_config.empty()) return;
  std::vector<std::pair<std::string, std::string>> values;
  for (const Row& row : Rows()) {
    if (rex::cvar::GetFlagInfo(row.cvar)) values.emplace_back(row.cvar, FileValue(row.cvar, Wanted(row)));
  }
  WriteConfigLines(g_config, values);
  REXLOG_INFO("menus: graphics settings saved to {}", g_config.string());
}

void Apply(const Row& row, const std::string& value) {
  const std::string name = row.cvar;
  if (name == "fullscreen") {
    // (the runtime's own handler of this setting moves the window: on the UI thread, like the F11 key)
    if (g_on_ui_thread) {
      g_on_ui_thread([value] {
        rex::cvar::SetFlagByName("fullscreen", value);
        REXLOG_INFO("menus: fullscreen = {}", value);
        std::lock_guard lock(g_settings_mutex);
        const auto it = g_wanted.find("fullscreen");
        if (it != g_wanted.end() && it->second == value) g_wanted.erase(it);
      });
    }
    return;
  }
  rex::cvar::SetFlagByName(name, value);
  if (name == "frame_rate_limit") {
    const int limit = std::atoi(value.c_str());
    rex::cvar::SetFlagByName("guest_vblank_hz", std::to_string(limit > 0 ? 2 * std::clamp(limit, 15, 500) : 1000));
  }
  REXLOG_INFO("menus: {} = {}", name, value);
}

// The settings' own thread: applies what was chosen once the presses have stopped, and keeps a change that the
// game has run with long enough.
void SettingsThread() {
  std::unique_lock lock(g_settings_mutex);
  for (;;) {
    const Clock::time_point next = std::min(g_apply_at, g_keep_at);
    if (next == Clock::time_point::max()) {
      g_settings_wake.wait(lock);
    } else {
      g_settings_wake.wait_until(lock, next);
    }
    const Clock::time_point now = Clock::now();
    if (now >= g_apply_at) {
      g_apply_at = Clock::time_point::max();
      std::vector<std::pair<const Row*, std::string>> changes;
      for (const Row& row : Rows()) {
        const auto it = g_wanted.find(row.cvar);
        if (it == g_wanted.end() || NextStart(row)) continue;
        changes.emplace_back(&row, it->second);
      }
      g_keep_at = Clock::now() + kKeepAfter;
      lock.unlock();
      for (const auto& [row, value] : changes) {
        if (rex::cvar::GetFlagByName(row->cvar) != value) Apply(*row, value);
      }
      lock.lock();
      // (the page shows the setting itself again - unless another press came meanwhile; fullscreen: see Apply)
      for (const auto& [row, value] : changes) {
        const auto it = g_wanted.find(row->cvar);
        if (it != g_wanted.end() && it->second == value && std::string(row->cvar) != "fullscreen") g_wanted.erase(it);
      }
    } else if (now >= g_keep_at && g_apply_at == Clock::time_point::max()) {
      g_keep_at = Clock::time_point::max();
      const size_t before = g_trials.size();
      std::erase_if(g_trials, [](const Trial& t) { return t.state != "next"; });
      if (g_trials.size() != before) {
        WriteTrials(g_config, g_trials);
        REXLOG_INFO("menus: the game ran with the changed graphics settings - kept ({} setting(s))", before - g_trials.size());
      }
    }
  }
}

// The program leaves in the ordinary way (not a crash, not a hung GPU): what was changed in this session worked.
void KeepOnExit() {
  std::lock_guard lock(g_settings_mutex);
  const size_t before = g_trials.size();
  std::erase_if(g_trials, [](const Trial& t) { return t.state != "next"; });
  if (g_trials.size() != before) WriteTrials(g_config, g_trials);
}

std::string RowPath(size_t row) { return "_root.SettingsGraphics_MC.SelectionBox_MC" + std::to_string(row + 1); }

// A row's tick box: hidden on rows that choose from a list.
void ShowTick(PPCContext& ctx, uint8_t* base, size_t index, const Row& row, bool on) {
  if (!row.tick) {
    SetMovieFlag(ctx, base, g_options_object, RowPath(index) + ".ToggleBox_MC._visible", false);
    return;
  }
  const std::string frame = on ? "On" : "Off";
  CallMovie(ctx, base, g_options_object, (RowPath(index) + ".ToggleBox_MC.gotoAndPlay").c_str(), &frame);
}

// The Graphics page asks for its rows.
void PopulateGraphics(PPCContext& ctx, uint8_t* base) {
  const std::vector<Row>& rows = Rows();
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::string text = RowText(rows[i]);
    CallMovie(ctx, base, g_options_object, "AddGraphicsListViewItem", &text);
    ShowTick(ctx, base, i, rows[i], Current(rows[i]) == 1);
  }
  SetMovieText(ctx, base, g_options_object, "_root.SettingsGraphics_MC.XMD_MENU_Titles_SubRight_MC.ItemTitle.text",
               "Graphics");
  // (the button legend as on the game's own pages: no "Select Storage Device" here; the game's CollapseSubMenu,
  // which the page calls on the way back, shows it again)
  SetMovieFlag(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.ItemTitle4._visible", false);
  SetMovieFlag(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.SaveButton._visible", false);
}

// Left / right / A on a row: the next choice (A and right: forward, around; left: back, around).
void ChangeGraphics(PPCContext& ctx, uint8_t* base, int index, int direction) {
  const std::vector<Row>& rows = Rows();
  if (index < 0 || size_t(index) >= rows.size()) return;
  const Row& row = rows[size_t(index)];
  const size_t count = row.choices.size();
    const size_t next = (Current(row) + (direction < 0 ? count - 1 : 1)) % count;
  const std::string value = row.choices[next].value, old_value = FileValue(row.cvar, Wanted(row));
  {
    // On trial first, then into the settings file; the setting itself changes when the presses have stopped.
    std::lock_guard lock(g_settings_mutex);
    const auto trial = std::find_if(g_trials.begin(), g_trials.end(), [&](const Trial& t) { return t.name == row.cvar; });
    if (trial == g_trials.end()) {
      g_trials.push_back({NextStart(row) ? "next" : "live", row.cvar, old_value});
    } else if (trial->old_value == FileValue(row.cvar, value)) {
      g_trials.erase(trial);  // (back at what it was)
    } else {
      trial->state = NextStart(row) ? "next" : "live";
    }
    WriteTrials(g_config, g_trials);
    g_wanted[row.cvar] = value;
    if (!NextStart(row)) {
      g_apply_at = Clock::now() + kApplyAfter;
      g_keep_at = Clock::time_point::max();
    }
  }
  g_settings_wake.notify_all();
  REXLOG_INFO("menus: {} chosen: {}{}", row.cvar, value, NextStart(row) ? " (for the next start)" : "");
  std::string text = row.tick ? std::string(row.label) + row.note
                              : std::string(row.label) + ":  " + row.choices[next].text + row.note;
  CallMovie(ctx, base, g_options_object, "SetGraphicsItem", &text, &index);
  if (row.tick) ShowTick(ctx, base, size_t(index), row, next == 1);
  SaveSettings();
}

// ---- the key bindings page ----
//
// Opened with X from the game's own Controls page (the legend's X entry reads "Customize" there). Rows: the
// device shown (keyboard and mouse / controller), every action of controls.h with its key or button, the mouse
// rows on the keyboard side, "Reset to Defaults". Eight slots show a window of them (g_bind_top = the first row
// shown). A on an action waits for the next key or button (controls::BeginCapture); the page's PCBindTick, every
// frame, sees it end. The bindings themselves, and saving them, are controls.cpp's.

constexpr int kBindSlots = menupatch::kGraphicsRows;
controls::Device g_bind_device = controls::Device::kKeyboardMouse;
int g_bind_top = 0, g_bind_waiting = -1;  // (g_bind_waiting: the row whose key is being waited for)

enum class BindRow { kDevice, kAction, kSensitivity, kInvertY, kReset };
struct BindEntry {
  BindRow kind;
  int action;
};
std::vector<BindEntry> BindRows() {
  std::vector<BindEntry> rows = {{BindRow::kDevice, 0}};
  for (int i = 0; i < controls::ActionCount(); ++i) rows.push_back({BindRow::kAction, i});
  if (g_bind_device == controls::Device::kKeyboardMouse) {
    rows.push_back({BindRow::kSensitivity, 0});
    rows.push_back({BindRow::kInvertY, 0});
  }
  rows.push_back({BindRow::kReset, 0});
  return rows;
}
std::string BindText(const BindEntry& row, bool waiting) {
  const bool keys = g_bind_device == controls::Device::kKeyboardMouse;
  switch (row.kind) {
    case BindRow::kDevice:
      return keys ? "Device:  Keyboard and Mouse" : "Device:  Controller";
    case BindRow::kAction:
      return std::string(controls::ActionName(row.action)) + ":  " +
             (waiting ? (keys ? "press a key..." : "press a button...") : controls::BindingText(row.action, g_bind_device));
    case BindRow::kSensitivity:
      return fmt::format("Mouse Sensitivity:  {:.1f}", controls::MouseSensitivity());
    case BindRow::kInvertY:
      return "Invert Mouse Y";
    case BindRow::kReset:
      return "Reset to Defaults";
  }
  return {};
}
std::string BindSlotPath(int slot) { return "_root.SettingsBindings_MC.SelectionBox_MC" + std::to_string(slot + 1); }

// The slots' texts and tick boxes for the rows now in view.
void ShowBindings(PPCContext& ctx, uint8_t* base, bool first) {
  const std::vector<BindEntry> rows = BindRows();
  g_bind_top = std::clamp(g_bind_top, 0, std::max(0, int(rows.size()) - kBindSlots));
  for (int slot = 0; slot < kBindSlots; ++slot) {
    const int index = g_bind_top + slot;
    const bool there = index < int(rows.size());
    const std::string text = there ? BindText(rows[size_t(index)], index == g_bind_waiting) : std::string();
    if (first) {
      CallMovie(ctx, base, g_options_object, "AddBindListViewItem", &text);
    } else {
      CallMovie(ctx, base, g_options_object, "SetBindItem", &text, &slot);
    }
    const bool tick = there && rows[size_t(index)].kind == BindRow::kInvertY;
    SetMovieFlag(ctx, base, g_options_object, BindSlotPath(slot) + ".ToggleBox_MC._visible", tick);
    if (tick) {
      const std::string frame = controls::MouseInvertY() ? "On" : "Off";
      CallMovie(ctx, base, g_options_object, (BindSlotPath(slot) + ".ToggleBox_MC.gotoAndPlay").c_str(), &frame);
    }
  }
}

// The legend's X entry: "Customize" on the game's Controls page, the game's own again afterwards.
void BorrowLegendX(PPCContext& ctx, uint8_t* base, bool borrow) {
  if (borrow == g_legend_x_borrowed) return;
  g_legend_x_borrowed = borrow;
  const bool shown = borrow || g_legend_x_shown;
  if (borrow || !g_legend_x_text.empty()) {
    SetMovieText(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.ItemTitle4.text",
                 borrow ? "Customize" : g_legend_x_text);
  }
  SetMovieFlag(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.ItemTitle4._visible", shown);
  SetMovieFlag(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.SaveButton._visible", shown);
}

// A call of the bindings page (PCBind...); `slot`: the highlighted slot, where the call gives one.
void OnBindingsCall(PPCContext& ctx, uint8_t* base, const std::string& name, int slot) {
  const std::vector<BindEntry> rows = BindRows();
  const int count = int(rows.size());
  if (name == "PCBindPopulate") {
    g_bind_device = controls::LastUsedDevice();
    g_bind_top = 0, g_bind_waiting = -1;
    controls::ClearCapture();
    ShowBindings(ctx, base, true);
    SetMovieText(ctx, base, g_options_object, "_root.SettingsBindings_MC.XMD_MENU_Titles_SubRight_MC.ItemTitle.text",
                 "Controls");
    // The legend: no X here, but "Select" and "Activate", which the game hid for its Controls page (its own
    // CollapseSubMenu shows everything again when the list is back).
    g_legend_x_borrowed = false;
    SetMovieFlag(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.ItemTitle4._visible", false);
    SetMovieFlag(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.SaveButton._visible", false);
    for (const char* part : {"ItemTitle1", "MovementIcon", "ItemTitle2", "ActivateButton"}) {
      SetMovieFlag(ctx, base, g_options_object, std::string("_root.SettingsButtonLegend_MC.") + part + "._visible", true);
    }
    if (!g_legend_x_text.empty()) {
      SetMovieText(ctx, base, g_options_object, "_root.SettingsButtonLegend_MC.ItemTitle4.text", g_legend_x_text);
    }
    return;
  }
  if (name == "PCBindTick") {
    if (g_bind_waiting < 0) return;
    const controls::Capture state = controls::CaptureState();
    if (state == controls::Capture::kWaiting) return;
    REXLOG_INFO("menus: binding of row {} {}", g_bind_waiting,
                state == controls::Capture::kDone ? "set to " + controls::CaptureText()
                : state == controls::Capture::kTimedOut ? std::string("timed out") : std::string("cancelled"));
    g_bind_waiting = -1;
    controls::ClearCapture();
    ShowBindings(ctx, base, false);  // (every row: a key another action had has swapped)
    return;
  }
  if (name == "PCBindClose") {
    if (g_bind_waiting >= 0) controls::CancelCapture(), controls::ClearCapture();
    g_bind_waiting = -1;
    return;
  }
  if (g_bind_waiting >= 0) return;  // (a key is being waited for: nothing else happens)
  if (name == "PCBindBack") {
    CallMovie(ctx, base, g_options_object, "BindLeave");
    return;
  }
  const int index = g_bind_top + slot;
  if (slot < 0 || slot >= kBindSlots || index >= count) return;
  if (name == "PCBindUp") {
    if (slot > 0) {
      CallMovie(ctx, base, g_options_object, "BindPrev");
    } else if (g_bind_top > 0) {
      --g_bind_top;
      ShowBindings(ctx, base, false);
    }
    return;
  }
  if (name == "PCBindDown") {
    if (slot < kBindSlots - 1 && index + 1 < count) {
      CallMovie(ctx, base, g_options_object, "BindNext");
    } else if (index + 1 < count) {
      ++g_bind_top;
      ShowBindings(ctx, base, false);
    }
    return;
  }
  const BindEntry& row = rows[size_t(index)];
  const bool press = name == "PCBindPress";
  const int direction = name == "PCBindLeft" ? -1 : 1;
  switch (row.kind) {
    case BindRow::kDevice:
      g_bind_device = g_bind_device == controls::Device::kPad ? controls::Device::kKeyboardMouse : controls::Device::kPad;
      g_bind_top = 0;
      break;
    case BindRow::kAction:
      if (!press || !controls::CanBind(row.action, g_bind_device)) return;
      if (!controls::BeginCapture(row.action, g_bind_device)) return;
      g_bind_waiting = index;
      break;
    case BindRow::kSensitivity: {
      const double now = std::round(controls::MouseSensitivity() * 10.0) / 10.0;
      double next = now + 0.1 * direction;
      if (press && next > 5.0 + 1e-9) next = 0.1;  // (A goes around)
      controls::SetMouseSensitivity(std::clamp(next, 0.1, 5.0));
      break;
    }
    case BindRow::kInvertY:
      controls::SetMouseInvertY(!controls::MouseInvertY());
      break;
    case BindRow::kReset:
      if (!press) return;
      controls::ResetDefaults(g_bind_device);
      REXLOG_INFO("menus: bindings reset to the defaults ({})",
                  g_bind_device == controls::Device::kPad ? "controller" : "keyboard and mouse");
      break;
  }
  ShowBindings(ctx, base, false);
}

// The changed movie in place of the disc's, for the memory file Scaleform is about to read.
void SwapMovie(uint8_t* base, uint32_t file) {
  if (!g_block || file < 0x10000 || Guest32(base, file) != kMemoryFileVtable) return;
  const uint32_t data = Guest32(base, file + 12), size = Guest32(base, file + 16);
  if (data < 0x10000 || size < 64 || size > 0x400000 || Guest32(base, file + 20) != 0) return;
  const std::string name = menupatch::MovieName(base + data, size);
  if (g_trace) REXLOG_INFO("menu trace: movie {} ({} bytes at {:08X})", name, size, data);
  const bool main_menu = name == "XMDMenuMain", options = name == "XMDMenuOptions";
  if (!main_menu && !options) return;
  // Debugging aids: XMD_MENU_ORIGINAL=1 - the menus as the disc has them, nothing added;
  // XMD_MENU_MOVIE_DIR=<folder> - <folder>\<movie name>.gfx, when there, instead of the change made here
  // (a movie written by tools\menu_patch_test.exe: a new layout tried without building the game).
  static const bool original = std::getenv("XMD_MENU_ORIGINAL") != nullptr;
  if (original) return;
  std::lock_guard lock(g_mutex);
  uint32_t& made = main_menu ? g_main_menu_size : g_options_size;
  const uint32_t at = g_block + kTextRoom + (main_menu ? 0 : kMainMenuRoom);
  const uint32_t room = main_menu ? kMainMenuRoom : kOptionsRoom;
  if (made == 0) {
    std::string why;
    std::vector<uint8_t> changed;
    if (const char* folder = std::getenv("XMD_MENU_MOVIE_DIR")) {
      std::ifstream in(std::filesystem::path(folder) / (name + ".gfx"), std::ios::binary);
      if (in) changed.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (!changed.empty()) REXLOG_INFO("menus: {} from {}", name, folder);
    }
    if (changed.empty()) {
      changed = main_menu ? menupatch::PatchMainMenu(base + data, size, &why)
                          : menupatch::PatchOptions(base + data, size, &why);
    }
    if (changed.empty() || changed.size() > room) {
      REXLOG_WARN("menus: {} stays as on the disc ({})", name, changed.empty() ? why : "the changed movie is too large");
      made = 1;
    } else {
      std::memcpy(base + at, changed.data(), changed.size());
      made = uint32_t(changed.size());
      REXLOG_INFO("menus: {} changed as it loads ({} -> {} bytes)", name, size, made);
    }
  }
  if (made <= 1) return;
  PutGuest32(base, file + 12, at);
  PutGuest32(base, file + 16, made);
}

}  // namespace

void Install(uint32_t guest_block, std::function<void()> exit_game,
             std::function<void(std::function<void()>)> on_ui_thread, std::filesystem::path config_file) {
  g_trace = std::getenv("XMD_MENU_TRACE") != nullptr;
  g_block = guest_block;
  g_exit = std::move(exit_game);
  g_on_ui_thread = std::move(on_ui_thread);
  g_config = std::move(config_file);
  for (const std::string& note : g_start_notes) REXLOG_WARN("menus: {}", note);
  {
    std::lock_guard lock(g_settings_mutex);
    g_trials = ReadTrials(g_config);
    if (!g_trials.empty()) g_keep_at = Clock::now() + kKeepAfterStart;  // ("trying": this start is their trial)
  }
  std::thread(SettingsThread).detach();
  std::atexit(KeepOnExit);
}

void RevertUnconfirmed(const std::filesystem::path& config_file) {
  std::vector<Trial> trials = ReadTrials(config_file), left;
  if (trials.empty()) return;
  std::vector<std::pair<std::string, std::string>> back;
  for (Trial& t : trials) {
    if (t.state == "next") {
      t.state = "trying";
      left.push_back(t);
    } else {
      back.emplace_back(t.name, t.old_value);
      g_start_notes.push_back("the last session ended before it had run with " + t.name + " changed - back to " + t.old_value);
    }
  }
  if (!back.empty()) WriteConfigLines(config_file, back);
  WriteTrials(config_file, left);
}

uint32_t GuestBlockSize() { return kTextRoom + kMainMenuRoom + kOptionsRoom; }

bool OnFlashCall(PPCContext& ctx, uint8_t* base) {
  const std::string name = GuestString(base, ctx.r5.u32, 80);
  const uint32_t values = ctx.r6.u32, count = ctx.r7.u32;
  if (g_trace && name != "PCBindTick") {
    REXLOG_INFO("menu trace: flash -> game {}({}) r3 {:08X} r4 {:08X}", name, Arguments(base, values, count),
                ctx.r3.u32, ctx.r4.u32);
  }
  if (name == "IISCompleted") g_title_ending = false;
  const auto number = [&](uint32_t i) {
    return i < count && (Guest32(base, values + i * 16 + 4) & 0xFF) == 3 ? int(GuestDouble(base, values + i * 16 + 8)) : -1000;
  };
  // The main menu's fifth entry, "Exit", asks first - with the menu's own second page, the one New Game uses
  // for the difficulty (a header and a short list; the game opens it with sub_824915C0 the same way): "Yes" /
  // "No", the movie's script puts the highlight on the second. While it is up the game hears nothing of it.
  if (g_main_menu_size > 1 && g_main_menu_object) {
    const uint32_t menu = g_main_menu_object;
    if (g_confirming_exit) {
      const bool chose = name == "OnSelectMenuOption" && count == 1;
      if (chose && number(0) == 0) {
        REXLOG_INFO("menus: Exit confirmed - closing the game");
        g_confirming_exit = false;
        KeepOnExit();  // (the runtime ends the process without the C runtime's exit handlers)
        if (g_exit) g_exit();
        return true;
      }
      if ((chose && number(0) == 1) || name == "GoBack") {
        REXLOG_INFO("menus: Exit declined");
        g_confirming_exit = false;
        SetMovieFlag(ctx, base, menu, "_root.DifficultyMenuNav3._visible", true);  // (the difficulty page's own)
        const std::string label = "MainMenuIntro";
        CallMovie(ctx, base, menu, "_root.gotoAndPlay", &label);
        return true;
      }
      if (chose || name == "GoToMapSelection") return true;
    } else if (name == "OnSelectMenuOption" && count == 1 && number(0) == 4 && g_exit_listed) {
      REXLOG_INFO("menus: Exit chosen on the main menu");
      SetMovieText(ctx, base, menu, "_root.PlayerDifficultyHeader_MC.ItemTitle.text", "Exit Game?");
      CallMovie(ctx, base, menu, "ClearDifficultyListViewItems");
      const std::string yes = "Yes", no = "No", label = "MainMenuOutro";
      CallMovie(ctx, base, menu, "_root.AddDifficultyListViewItem", &yes);
      CallMovie(ctx, base, menu, "_root.AddDifficultyListViewItem", &no);
      // (two entries in a list of three slots: the third button, still holding the last difficulty's name, hides)
      CallMovie(ctx, base, menu, "_root.mDifficultyListView.UpdateText");
      SetMovieFlag(ctx, base, menu, "_root.DifficultyMenuNav3._visible", false);
      const int no_entry = 1;
      CallMovie(ctx, base, menu, "_root.mDifficultyListView.OnUpdate", nullptr, &no_entry);
      CallMovie(ctx, base, menu, "_root.gotoAndPlay", &label);
      g_confirming_exit = true;
      return true;
    }
  }
  // Options: the sixth entry opens the Graphics page (the game never hears of index 5).
  if (g_options_size > 1 && g_options_object) {
    if (name == "SelectMenuOption" && count == 1 && number(0) == 5) {
      const std::string label = "Graphics";
      CallMovie(ctx, base, g_options_object, "_root.gotoAndStop", &label);
      return true;
    }
    if (name == "PCGraphicsPopulate") {
      PopulateGraphics(ctx, base);
      return true;
    }
    if (count == 1 && (name == "PCGraphicsLeft" || name == "PCGraphicsRight" || name == "PCGraphicsPress")) {
      ChangeGraphics(ctx, base, number(0), name == "PCGraphicsLeft" ? -1 : 1);
      return true;
    }
    if (name == "PCGraphicsClose") return true;
    if (name.rfind("PCBind", 0) == 0) {
      OnBindingsCall(ctx, base, name, count == 1 ? number(0) : -1);
      return true;
    }
    // The game's Controls page: X opens the bindings page, and the legend says so.
    if (name == "PopulateControlsMenu") BorrowLegendX(ctx, base, true);
    if (name == "CollapseSubMenu") BorrowLegendX(ctx, base, false);
  }
  return name.rfind("PCGraphics", 0) == 0 || name.rfind("PCBind", 0) == 0;
}

}  // namespace xmd::pcmenu

// The game calling a function of a movie: r3 = the game's object of the movie, r4 = the function's path.
REX_HOOK_RAW(sub_82718CF0) {
  using namespace xmd::pcmenu;
  const uint32_t object = ctx.r3.u32;
  if (g_trace) {
    REXLOG_INFO("menu trace: game -> flash {}({}) object {:08X}", GuestString(base, ctx.r4.u32),
                Arguments(base, Guest32(base, object + 464), Guest32(base, object + 468)), object);
  }
  // The title screen's "button pressed" frames start (see the head of this file) / the title screen is back.
  if (Guest32(base, object + 468) == 1 && GuestString(base, ctx.r4.u32, 64) == "_root.gotoAndPlay") {
    const uint32_t value = Guest32(base, object + 464);
    if (value >= 0x10000 && (Guest32(base, value + 4) & 0xFF) == 4) {
      const std::string label = GuestString(base, Guest32(base, value + 8), 32);
      if (label == "IISButtonPress") g_title_ending = true;
      if (label == "MainMenuStart" || label == "IISScreen") g_title_ending = false;
    }
  }
  // (a list being emptied is no longer the main menu's five: another page of the movie may list other things)
  if (g_exit_listed && object == g_main_menu_object && GuestString(base, ctx.r4.u32, 64) == "ClearMainListViewItems") {
    g_exit_listed = false;
  }
  // The main menu's list is complete: one more entry, "Exit" (only when the movie has the button for it).
  if (g_filling_main_menu && g_main_menu_size > 1 && Guest32(base, object + 468) == 0 &&
      GuestString(base, ctx.r4.u32, 64) == "_root.mMainListView.UpdateText") {
    const std::string exit_text = "Exit";
    CallMovie(ctx, base, object, "_root.AddMainListViewItem", &exit_text);
    g_main_menu_object = object;
    g_exit_listed = true;
    g_confirming_exit = false;
  }
  __imp__sub_82718CF0(ctx, base);
}

// The game setting a variable of a movie (r4 = its path) to a text (r5, 16-bit characters) / to a boolean.
REX_HOOK_RAW(sub_82718900) {
  if (xmd::pcmenu::g_trace) {
    REXLOG_INFO("menu trace: game -> flash {} = L\"{}\" object {:08X}", xmd::pcmenu::GuestString(base, ctx.r4.u32),
                xmd::pcmenu::GuestWideString(base, ctx.r5.u32), ctx.r3.u32);
  }
  if (xmd::pcmenu::GuestString(base, ctx.r4.u32, 64) == "_root.SettingsButtonLegend_MC.ItemTitle4.text") {
    xmd::pcmenu::g_legend_x_text = xmd::pcmenu::GuestWideString(base, ctx.r5.u32);
    xmd::pcmenu::g_legend_x_borrowed = false;
  }
  __imp__sub_82718900(ctx, base);
}
REX_HOOK_RAW(sub_82718748) {
  if (xmd::pcmenu::GuestString(base, ctx.r4.u32, 64) == "_root.Settings_MC.Settings_Nav_MC5._visible") {
    xmd::pcmenu::g_difficulty_hidden = (ctx.r5.u32 & 0xFF) == 0;
  }
  if (const std::string path = xmd::pcmenu::GuestString(base, ctx.r4.u32, 64);
      path == "_root.SettingsButtonLegend_MC.ItemTitle4._visible" || path == "_root.SettingsButtonLegend_MC.SaveButton._visible") {
    if (xmd::pcmenu::g_legend_x_borrowed) {
      ctx.r5.u64 = 1;  // (the game hides the whole legend but "Back" on its Controls page: X stays, it opens the bindings)
    } else if (path.find("ItemTitle4") != std::string::npos) {
      xmd::pcmenu::g_legend_x_shown = (ctx.r5.u32 & 0xFF) != 0;
    }
  }
  if (xmd::pcmenu::g_trace) {
    REXLOG_INFO("menu trace: game -> flash {} = {} object {:08X}", xmd::pcmenu::GuestString(base, ctx.r4.u32),
                ctx.r5.u32 & 0xFF, ctx.r3.u32);
  }
  __imp__sub_82718748(ctx, base);
}

// Debugging aid (XMD_MENU_TRACE): who called - the return addresses up the game's stack. Every function of the
// game keeps its caller's return address 8 bytes under the stack pointer it was entered with, and each stack
// frame starts with the address of the one before it.
namespace xmd::pcmenu {
void LogCallers(const char* what, PPCContext& ctx, uint8_t* base) {
  if (!g_trace) return;
  std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t frame = ctx.r1.u32;
  // (the stack ends at the next 64 KB boundary at the latest for this walk: nothing is read at or above it)
  const uint32_t top = (frame | 0xFFFF) + 1;
  for (int i = 0; i < 14 && frame >= 0x10000 && frame + 4 <= top; ++i) {
    const uint32_t above = Guest32(base, frame);
    if (above <= frame || above + 4 > top) break;
    chain += fmt::format(" {:08X}", Guest32(base, above - 8));
    frame = above;
  }
  REXLOG_INFO("menu trace: {} (r3 {:08X}) called from {}", what, ctx.r3.u32, chain);
}
}  // namespace xmd::pcmenu

// The Options menu closing (its own function for it) and the main menu taking over again from Options.
REX_HOOK_RAW(sub_824B27A8) {
  xmd::pcmenu::LogCallers("Options closes", ctx, base);
  __imp__sub_824B27A8(ctx, base);
}
REX_HOOK_RAW(sub_82492AA8) {
  xmd::pcmenu::LogCallers("main menu back from Options", ctx, base);
  __imp__sub_82492AA8(ctx, base);
}

// The two functions that fill the main menu's list.
REX_HOOK_RAW(sub_824920F8) {
  xmd::pcmenu::LogCallers("main menu list filled (in place)", ctx, base);
  ++xmd::pcmenu::g_filling_main_menu;
  __imp__sub_824920F8(ctx, base);
  --xmd::pcmenu::g_filling_main_menu;
}
REX_HOOK_RAW(sub_82492348) {
  using namespace xmd::pcmenu;
  LogCallers("main menu list filled, intro played", ctx, base);
  // The movie is about to be sent on to the main menu while the title screen's last frames still play: their
  // IISCompleted will not come, so the game's handler for it runs now (it fills the list itself, through this
  // hook again; it also clears [menu + 428], "there are saves", which the caller has just set - put back).
  if (g_title_ending) {
    g_title_ending = false;
    const uint32_t menu = ctx.r3.u32;
    const uint32_t saves = Guest32(base, menu + 428);
    const PPCContext saved = ctx;
    ctx.r1.u32 -= 512;
    sub_8248EDE8(ctx, base);
    ctx = saved;
    PutGuest32(base, menu + 428, saves);
    REXLOG_INFO("menus: the title screen's IISCompleted delivered by the port (the saves were listed first)");
  }
  ++g_filling_main_menu;
  __imp__sub_82492348(ctx, base);
  --g_filling_main_menu;
}

// The function that fills the Options list (r3 = the menu; its movie object at [+340]): "Graphics" after the
// game's five entries, when the movie has the sixth button.
REX_HOOK_RAW(sub_824B2EB0) {
  using namespace xmd::pcmenu;
  const uint32_t object = Guest32(base, ctx.r3.u32 + 340);
  LogCallers("Options list filled", ctx, base);
  g_difficulty_hidden = false;
  __imp__sub_824B2EB0(ctx, base);
  if (g_options_size > 1 && object >= 0x10000) {
    const std::string text = "Graphics";
    CallMovie(ctx, base, object, "AddMainListViewItem", &text);
    // (on the main menu the game hides the fifth entry, Difficulty: the sixth button moves up into its place)
    SetMovieText(ctx, base, object, "_root.Settings_MC.Settings_Nav_MC6._y",
                 fmt::format("{:.2f}", xmd::menupatch::OptionsButtonY(g_difficulty_hidden ? 4 : 5)));
    g_options_object = object;
  }
}

// Scaleform starting to read a movie: r4 = the file.
REX_HOOK_RAW(sub_827D99E8) {
  xmd::pcmenu::SwapMovie(base, ctx.r4.u32);
  __imp__sub_827D99E8(ctx, base);
}
