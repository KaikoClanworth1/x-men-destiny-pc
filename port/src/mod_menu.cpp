// X-Men: Destiny - the mod menu (see mod_menu.h).
//
// How each thing is done (found 2026-11-20, tools: the image's function table {function, name[64]} at 0x8339C110,
// the mission scripts in runs\lua, the game's database archive databases.zzz):
//
//  * Script functions: the game's Lua state still has its developer console - RestoreMainPlayerHealth,
//    RestoreMPower, AddXP, givemutator, killallenemies, DisableAI / EnableAI, settimedialation, showhud,
//    hidemainplayer, EquipFansuit, spawnmainplayer, load, cineplexaction ... Run through debug_commands.cpp.
//  * Checkpoints: the game's own Mission Select menu (CXMDMenuLoadCharacter, StartMission = sub_824B1EA0) writes
//    the checkpoint's name into the game object ([0x836684F8] + 0x710, the engine's string class), clears +0x7FC,
//    sets +0x804 and the save manager's +0x138, then does what the script function LoadCheckpoint() does. The
//    names are the "CheckPoints" table's MapName column in databases.zzz. Warp() below does the same.
//  * Cut-scenes: an in-engine scene is a "cineplex" actor inside a level archive; the mission scripts start it
//    with cineplexaction("<actor id>", 0) after load("<archive>"). The list (mod_menu_tables.inc) is generated
//    from the scripts by tools\mod_menu_tables.py, which keeps the scenes tools\mod_sweep.ps1 saw play
//    (tools\mod_menu_scenes.txt) with the checkpoint each played from: an entry loads that checkpoint, then
//    starts the scene. While a scene plays the game runs none of the commands waiting (nor this menu's).
//  * All 93 checkpoints (missions 1-7, 15 arenas) were loaded one after another by tools\mod_sweep.ps1 (2026-11-20).
//  * Tried and left out (2026-11-20): killallenemies() kills nobody in a fight and killall(1) removes the player
//    too; EquipFansuit / UnequipFansuit with every name form change nothing (suits: unlock them, wear them in the
//    pause menu); spawnmainplayer("AXMDCyclopsBody") - a main player of an X-Man's class - crashes in the
//    actor's set-up; moveplayer() does not move.
//  * Movies (.usm): the only caller is the main menu's cinema page (sub_824DCFD0, a CNotificationMessage_
//    ScaleformVideoPlay to the menu object at game + 0xCC). In a level there is no such object and the message
//    crashes the game (tried 2026-11-20) - not offered.

#include "mod_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "debug_commands.h"
#include "generated/default/xmd_init.h"
#include "perf_overlay.h"

REX_EXTERN(sub_826ED400);  // string(this, const char*)
REX_EXTERN(sub_826ED560);  // string = string
REX_EXTERN(sub_824DA3C8);  // ~string
REX_EXTERN(sub_8251CDF0);  // game -> the save manager's current profile record
REX_EXTERN(sub_82339B20);  // game state: the mission number (0-7)
REX_EXTERN(sub_823ED540);  // loader->Load(name, slot): what the script function LoadCheckpoint() calls

namespace xmd::modmenu {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kGameState = 0x836684F8;  // (the same object as 0x8366855C in a running game)
constexpr uint32_t kGame = 0x8366855C;
constexpr uint32_t kScratchSize = 0x400;

std::filesystem::path g_user_data;
rex::input::InputSystem* g_input = nullptr;
uint32_t g_scratch = 0;

std::atomic<bool> g_open{false};
std::atomic<bool> g_god{false}, g_power{false}, g_enemies_frozen{false}, g_hud_hidden{false},
    g_player_hidden{false}, g_enemies_slow{false};
std::atomic<float> g_speed{1.0f};
std::atomic<uint16_t> g_pad_seen{0};  // (the controller's buttons as the game last got them: menu closed)

// ---- the game's side -------------------------------------------------------------------------------------------

void Lua(const std::string& code) { debug::RunLua(code); }
// (no log line: the cheats that repeat four times a second)
void QuietLua(const std::string& code) {
  debug::Query(code + " return 1", [](bool, const std::string&) {});
}

void Later(int ms, std::function<void()> work);

uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return _byteswap_ulong(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + address, &v, 4);
}

// A checkpoint load asked for in the middle of a level ("warp").
//
// The game's script function LoadCheckpoint() is one call, loader->Load(record's name, record's slot)
// (sub_823ED540, the function at +0x48 of the object at game + 0xB8), and that answers whether it took the request:
// it refuses while the save manager is busy and in some states of a challenge arena, and a refused request leaves
// the player where they are. So the warp makes that call itself and looks at the answer. A request the game did
// not start is tried again by way of Mission 1's start, which loaded from every place tried; from inside an arena
// that way is taken at once (the game does not start a load from one arena to another).
//
// Afterwards the picture stays faded to black (the game's own way in is a loading screen that fades in): the ticker
// waits for the old world to go (a position question that fails, or one that waits more than a second and a half
// for its answer - no scripts run during a load) and for the player to be back and still for two seconds, then
// fades in as the mission scripts do.
std::atomic<int> g_warp_phase{0}, g_warp_ticks{0}, g_still_ticks{0};
std::atomic<bool> g_warp_away{false}, g_warp_asked{false}, g_warp_refused{false};
std::mutex g_warp_mutex;
std::string g_warp_before, g_warp_last;
// (what to load once the detour by Mission 1 is there; and whether the load running is such a detour or retry)
std::string g_warp_then, g_warp_name;
int g_warp_then_mission = -1, g_warp_mission = -1;
bool g_warp_direct = false;
// (what follows a warp once the place is there: a cut-scene's start)
std::function<void()> g_warp_after;
Clock::time_point g_warp_started;

bool SamePlace(const std::string& a, const std::string& b, float tolerance) {
  float p[3], q[3];
  if (std::sscanf(a.c_str(), "%f %f %f", &p[0], &p[1], &p[2]) != 3) return false;
  if (std::sscanf(b.c_str(), "%f %f %f", &q[0], &q[1], &q[2]) != 3) return false;
  return std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]) + std::fabs(p[2] - q[2]) < tolerance;
}
const char kFadeIn[] =
    "SetPPFadeInOutByName(\"CA83112C-D205-45EE-9975-D41BCED964DD\",0,0,0,1,0,0,0,0,1) showhud(true)";
const char kDetour[] = "Mission1";

// The checkpoint the game is at (the text of the string at game + 0x710), "" when unknown.
std::string CurrentCheckpoint(uint8_t* base, uint32_t state) {
  const uint32_t text = Load32(base, state + 0x710), size = Load32(base, state + 0x714);
  if (text < 0x10000 || size == 0 || size > 80) return {};
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(base + text, &info, sizeof(info)) || info.State != MEM_COMMIT) return {};
  std::string name(reinterpret_cast<const char*>(base + text), strnlen(reinterpret_cast<const char*>(base + text), size));
  for (char c : name) {
    if (c < 32 || c > 126) return {};
  }
  return name;
}

// What the Mission Select menu's StartMission does with the entry chosen, then the load.
// `direct`: no detour (the detour itself, and the load after it).
void Warp(const std::string& checkpoint, int mission, bool direct = false, std::function<void()> after = {}) {
  if (!g_scratch || checkpoint.size() + 40 > kScratchSize) return;
  {
    std::lock_guard lock(g_warp_mutex);
    g_warp_name = checkpoint;
    g_warp_mission = mission;
    g_warp_direct = direct;
    if (!direct) {
      g_warp_then.clear();
      g_warp_after = std::move(after);
    }
  }
  g_warp_ticks = 0;
  g_still_ticks = 0;
  g_warp_away = false;
  g_warp_asked = false;
  g_warp_refused = false;
  g_warp_started = Clock::now();
  debug::QueryPosition([](bool ok, const std::string& text) {
    std::lock_guard lock(g_warp_mutex);
    g_warp_before = ok ? text : std::string();
    g_warp_last.clear();
  });
  debug::RunNative([checkpoint, mission, direct](PPCContext& ctx, uint8_t* base) {
    const uint32_t state = Load32(base, kGameState), game = Load32(base, kGame);
    if (state < 0x10000 || game < 0x10000) {
      REXLOG_WARN("mod menu: no game object yet - checkpoint {} not loaded", checkpoint);
      g_warp_refused = true;
      g_warp_asked = true;
      return;
    }
    const std::string here = CurrentCheckpoint(base, state);
    if (!direct && !here.empty() && std::isdigit(static_cast<unsigned char>(here[0])) && checkpoint != kDetour) {
      REXLOG_INFO("mod menu: in the arena {} - to {} by way of {}", here, checkpoint, kDetour);
      g_warp_refused = true;  // (the ticker starts the detour)
      g_warp_asked = true;
      return;
    }
    // [0] a string object (12 bytes), [32] the text
    std::memset(base + g_scratch, 0, 32);
    std::memcpy(base + g_scratch + 32, checkpoint.c_str(), checkpoint.size() + 1);
    ctx.r3.u64 = g_scratch;
    ctx.r4.u64 = g_scratch + 32;
    sub_826ED400(ctx, base);
    ctx.r3.u64 = state + 0x710;
    ctx.r4.u64 = g_scratch;
    sub_826ED560(ctx, base);
    ctx.r3.u64 = g_scratch;
    sub_824DA3C8(ctx, base);
    if (mission >= 0 && mission <= 7) {
      ctx.r3.u64 = state;
      ctx.r4.u64 = uint32_t(mission);
      ctx.r5.u64 = uint32_t(mission);
      sub_82339B20(ctx, base);
    }
    Store32(base, state + 0x7FC, 0);
    Store32(base, state + 0x804, 1);
    // (the loader's own state: left at 5 by a load that did not come from a menu, and a second one is then
    // ignored - found by comparing two F4 state captures)
    Store32(base, state + 0x70C, 0);
    ctx.r3.u64 = game;
    sub_8251CDF0(ctx, base);
    const uint32_t record = ctx.r3.u32;
    if (record >= 0x10000) Store32(base, record + 0x138, 1);
    // LoadCheckpoint(): loader->Load(record's name, record's slot)
    const uint32_t loader = Load32(base, game + 0xB8), saves = Load32(base, game + 0x1D0);
    uint32_t answer = 0;
    if (record >= 0x10000 && loader >= 0x10000 && Load32(base, Load32(base, loader) + 0x48) == 0x823ED540) {
      ctx.r3.u64 = loader;
      ctx.r4.u64 = Load32(base, record + 0x10);
      ctx.r5.u64 = Load32(base, record + 0x68);
      sub_823ED540(ctx, base);
      answer = ctx.r3.u32;
    }
    REXLOG_INFO("mod menu: checkpoint {} (mission {}) from {}: the game {} (slot {}, save manager busy {})",
                checkpoint, mission, here.empty() ? "?" : here.c_str(), answer ? "took the request" : "REFUSED it",
                record >= 0x10000 ? Load32(base, record + 0x68) : 0, saves >= 0x10000 ? Load32(base, saves + 0x1C8) : 0);
    if (!answer) g_warp_refused = true;
    g_warp_asked = true;
  });
  g_warp_phase = 1;
}

void EndWarp(bool loaded, const std::string& notice) {
  g_warp_phase = 0;
  // (back to a campaign game, as before the load: StartMission's two flags mean "from Mission Select")
  debug::RunNative([](PPCContext&, uint8_t* base) {
    const uint32_t state = Load32(base, kGameState);
    if (state < 0x10000) return;
    Store32(base, state + 0x7FC, 1);
    Store32(base, state + 0x804, 0);
    Store32(base, state + 0x70C, 0);
  });
  if (loaded) Lua(kFadeIn);
  ShowNotice(notice, loaded ? 2.0f : 6.0f);
  std::function<void()> after;
  {
    std::lock_guard lock(g_warp_mutex);
    after.swap(g_warp_after);
  }
  if (loaded && after) after();
}

// A quarter of a second of a warp (the ticker's).
void WarpTick() {
  // (not piled up while the game runs no scripts - a loading screen)
  if (debug::Waiting()) return;
  std::string name, then;
  int mission, then_mission;
  bool direct;
  {
    std::lock_guard lock(g_warp_mutex);
    name = g_warp_name, mission = g_warp_mission, direct = g_warp_direct;
    then = g_warp_then, then_mission = g_warp_then_mission;
  }
  if (!g_warp_asked.load()) {
    // (the request not made yet: no script ran - four minutes of that, and it is given up)
    if (++g_warp_ticks > 960) EndWarp(false, "The game runs no scripts now - " + name + " not loaded");
    return;
  }
  // Not started: refused outright, or the player still on the old spot fifteen seconds after a request the game
  // took (the old world goes at once when a load does start).
  bool late = false;
  if (g_warp_ticks.load() > 60) {
    std::lock_guard lock(g_warp_mutex);
    late = !g_warp_last.empty() && SamePlace(g_warp_last, g_warp_before, 30.0f);
  }
  if (!g_warp_away.load() && (g_warp_refused.load() || late)) {
    if (!direct && name != kDetour) {
      REXLOG_INFO("mod menu: {} did not start - by way of {}", name, kDetour);
      {
        std::lock_guard lock(g_warp_mutex);
        g_warp_then = name;
        g_warp_then_mission = mission;
      }
      Warp(kDetour, 0, true);
      return;
    }
    REXLOG_WARN("mod menu: checkpoint {} NOT loaded: the game did not start the load", name);
    {
      std::lock_guard lock(g_warp_mutex);
      g_warp_then.clear();
    }
    EndWarp(false, "The game did not load " + name + " now (a cut-scene or a save running?) - try again");
    return;
  }
  const int ticks = ++g_warp_ticks;
  const auto asked = Clock::now();
  debug::QueryPosition([asked](bool ok, const std::string& text) {
    std::lock_guard lock(g_warp_mutex);
    if (!ok || Clock::now() - asked > std::chrono::milliseconds(1500)) g_warp_away = true;
    if (ok && g_warp_away.load() && SamePlace(text, g_warp_last, 1.0f)) ++g_still_ticks;
    else g_still_ticks = 0;
    g_warp_last = ok ? text : std::string();
  });
  // (a minute for a load that began and whose world never seemed away; four minutes at most: a slow disk)
  bool player;
  {
    std::lock_guard lock(g_warp_mutex);
    player = !g_warp_last.empty();
  }
  if (g_still_ticks >= 8 || (player && !g_warp_away.load() && ticks > 240) || ticks > 960) {
    if (!then.empty() && name == kDetour) {
      // (the detour is there: now the place asked for)
      {
        std::lock_guard lock(g_warp_mutex);
        g_warp_then.clear();
      }
      REXLOG_INFO("mod menu: at {}, now {}", kDetour, then);
      Warp(then, then_mission, true);
      return;
    }
    REXLOG_INFO("mod menu: checkpoint {} loaded", name);
    EndWarp(true, "Checkpoint loaded");
  }
}

// ---- things done later, and the cheats that are kept up -------------------------------------------------------

std::mutex g_later_mutex;
std::deque<std::pair<Clock::time_point, std::function<void()>>> g_later;

void Later(int ms, std::function<void()> work) {
  std::lock_guard lock(g_later_mutex);
  g_later.emplace_back(Clock::now() + std::chrono::milliseconds(ms), std::move(work));
}

void Ticker() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::vector<std::function<void()>> due;
    {
      std::lock_guard lock(g_later_mutex);
      const auto now = Clock::now();
      for (auto it = g_later.begin(); it != g_later.end();) {
        if (it->first <= now) {
          due.push_back(std::move(it->second));
          it = g_later.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& work : due) work();
    if (g_warp_phase.load()) WarpTick();
    if (debug::Waiting()) continue;
    // (not during a warp: nothing of the cheats' is run while the world is taken down and put up)
    if (g_warp_phase.load()) continue;
    if (g_god) QuietLua("RestoreMainPlayerHealth()");
    if (g_power) QuietLua("RestoreMPower()");
  }
}

// ---- saved spots -----------------------------------------------------------------------------------------------

constexpr int kSpots = 8;
struct Spot {
  bool used = false;
  float x = 0, y = 0, z = 0;
};
std::mutex g_spots_mutex;
Spot g_spots[kSpots];

void LoadSpots() {
  std::ifstream in(g_user_data / "mod_spots.txt");
  int n;
  Spot s;
  std::lock_guard lock(g_spots_mutex);
  while (in >> n >> s.x >> s.y >> s.z) {
    s.used = true;
    if (n >= 1 && n <= kSpots) g_spots[n - 1] = s;
  }
}

void SaveSpots() {
  std::error_code ec;
  std::filesystem::create_directories(g_user_data, ec);
  std::ofstream out(g_user_data / "mod_spots.txt");
  std::lock_guard lock(g_spots_mutex);
  for (int i = 0; i < kSpots; ++i) {
    if (g_spots[i].used) out << i + 1 << " " << g_spots[i].x << " " << g_spots[i].y << " " << g_spots[i].z << "\n";
  }
}

void SaveSpot(int n) {
  debug::QueryPosition([n](bool ok, const std::string& text) {
    Spot s;
    if (!ok || std::sscanf(text.c_str(), "%f %f %f", &s.x, &s.y, &s.z) != 3) {
      ShowNotice("No player in the world - spot not saved", 3.0f);
      REXLOG_WARN("mod menu: spot {} not saved ({})", n, text);
      return;
    }
    s.used = true;
    {
      std::lock_guard lock(g_spots_mutex);
      g_spots[n - 1] = s;
    }
    SaveSpots();
    REXLOG_INFO("mod menu: spot {} saved at {}", n, text);
    ShowNotice("Spot " + std::to_string(n) + " saved: " + text, 3.0f);
  });
}

void GotoSpot(int n) {
  Spot s;
  {
    std::lock_guard lock(g_spots_mutex);
    s = g_spots[n - 1];
  }
  if (!s.used) {
    ShowNotice("Spot " + std::to_string(n) + " is empty", 2.0f);
    return;
  }
  debug::GotoPosition(s.x, s.y, s.z, [n](bool ok, const std::string& text) {
    REXLOG_INFO("mod menu: spot {}: {}{}", n, ok ? "now at " : "failed: ", text);
    ShowNotice(ok ? "At spot " + std::to_string(n) : "No player in the world", 2.0f);
  });
}

// ---- the entries -----------------------------------------------------------------------------------------------

struct Entry {
  std::string id;     // (for "mod run <id>")
  std::string label;
  std::string note;   // (shown under the list while the entry is selected)
  std::function<void()> run;
  std::function<bool()> on;  // (a switch: its state)
  int page = -1;             // (opens that page)
};
struct Page {
  std::string title;
  std::vector<Entry> entries;
  int selected = 0;
};
std::vector<Page> g_pages;       // (built once in Build; g_pages[0] is the first page)
std::mutex g_ui_mutex;
std::vector<int> g_stack{0};     // (the pages gone into)
std::deque<std::string> g_keys;  // ("mod key ..." steps waiting for the next frame)

Entry* Find(const std::string& id) {
  for (Page& page : g_pages) {
    for (Entry& e : page.entries) {
      if (e.id == id) return &e;
    }
  }
  return nullptr;
}

int AddPage(const std::string& title) {
  g_pages.push_back(Page{title, {}, 0});
  return int(g_pages.size()) - 1;
}
void Link(int from, int to, const std::string& id, const std::string& note = "") {
  Entry e;
  e.id = id;
  e.label = g_pages[to].title;
  e.note = note;
  e.page = to;
  g_pages[from].entries.push_back(std::move(e));
}
void Action(int page, const std::string& id, const std::string& label, const std::string& note,
            std::function<void()> run) {
  g_pages[page].entries.push_back(Entry{id, label, note, std::move(run), nullptr, -1});
}
void Switch(int page, const std::string& id, const std::string& label, const std::string& note,
            std::atomic<bool>* flag, std::function<void(bool)> changed) {
  Entry e;
  e.id = id;
  e.label = label;
  e.note = note;
  e.on = [flag] { return flag->load(); };
  e.run = [flag, changed, label] {
    const bool now = !flag->load();
    *flag = now;
    if (changed) changed(now);
    ShowNotice(label + (now ? ": on" : ": off"), 2.0f);
  };
  g_pages[page].entries.push_back(std::move(e));
}

std::string Slug(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) out += char(std::tolower(static_cast<unsigned char>(c)));
    else if (!out.empty() && out.back() != '_') out += '_';
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

// "M1_FB_bIceCombatCompleted" -> "Ice Combat Completed"
std::string Pretty(std::string name) {
  static const char* const prefixes[] = {"M1_FB_b", "M2_FB_", "M3_CT_354_", "M3_CT_", "M3_Lab_", "M4_Fissure_",
                                         "M5_BZ_b", "M5_BZ_", "M6_BZ_", "M7_Soma_"};
  for (const char* p : prefixes) {
    if (name.rfind(p, 0) == 0) {
      name.erase(0, std::strlen(p));
      break;
    }
  }
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '_') {
      out += ' ';
      continue;
    }
    if (i && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(name[i - 1])))
      out += ' ';
    if (i && std::isdigit(static_cast<unsigned char>(c)) && std::isalpha(static_cast<unsigned char>(name[i - 1])))
      out += ' ';
    out += c;
  }
  if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

struct Mission {
  const char* title;
  int number;  // (the Mission Select list's index)
  std::vector<const char*> checkpoints;  // (the first is the mission's start)
};
const std::vector<Mission>& Missions() {
  static const std::vector<Mission> missions = {
      {"Mission 1 - Ferry Building", 0,
       {"Mission1", "M1_FB_bQSIntroCompleted", "M1_FB_bQSCombatCompleted", "M1_FB_bIceIntroCompleted",
        "M1_FB_bIceCombatCompleted", "M1_FB_bEnteredSewer", "M1_FB_bEmmaBattleStarted",
        "M1_FB_bEmmaBattleCompleted"}},
      {"Mission 2 - Mutant Annex", 1,
       {"Mission2_Part1", "M2_FB_HodgeIntroPlayed", "M2_FB_HodgeGoonsKilled", "M2_FB_TalkedtoPyro",
        "M2_FB_ForgeIntro", "M2_FB_HelpForge", "M2_FB_RadioTower", "M2_FB_CyclopsIntro", "M2_FB_FinalBattle",
        "M2_FB_FinalBattleDone", "M2_FB_HodgeFight"}},
      {"Mission 3 - Chinatown", 2,
       {"M3_CT_Terminal", "M3_CT_CourierIntro", "M3_CT_CourierStart", "M3_CT_CourierRooftop", "M3_CT_CourierBrawl",
        "M3_CT_CourierEnd", "M3_CT_MarketplaceStart", "M3_CT_354_MarketplaceEnd", "M3_CT_GambitFight",
        "M3_CT_GambitEnd", "M3_CT_outsideWarehouse", "M3_CT_insideWarehouse", "M3_CT_PressHoist",
        "M3_CT_WarehouseComplete"}},
      {"Mission 3 - Purifier Lab", 3,
       {"Purifier_Lab_New", "M3_Lab_AfterQS", "M3_Lab_AfterDegen", "M3_Lab_BeforeCleansers", "M3_Lab_MechFight",
        "M3_Lab_SurgeBreakout", "M3_Lab_ColossusBreakout", "M3_Lab_QuicksilverBreakout", "M3_Lab_XmenTrapped",
        "M3_Lab_ScientistBoss"}},
      {"Mission 4 - Subway Fissure", 4,
       {"XMD_SUB_Fissure", "M4_Fissure_GroundPound", "M4_Fissure_AfterSniffing", "M4_Fissure_AfterLuis",
        "M4_Fissure_BridgeCross", "M4_Fissure_BrokenBridge", "M4_Fissure_BeforeMech", "M4_Fissure_BeforeHodge"}},
      {"Mission 5 - Buffer Zone", 5,
       {"Mission5_BufferZone_Part1Dave", "M5_BZ_ConversationOver", "M5_BZ_SpawnTheMech", "M5_BZ_MechDestroyed",
        "M5_BZ_BuildingEnd", "M5_BZ_ShippingCratesEnd", "M5_BZ_bWingsStarted", "M5_BZ_bWolverineStarted",
        "M5_BZ_bRoofPixieStarted", "M5_BZ_bRoof02Started", "M5_BZ_bCrane01Started", "M5_BZ_bRescuePixieStarted",
        "M5_BZ_RescuePixieEnded", "M5_BZ_bMagJugBossStarted"}},
      {"Mission 6 - Buffer Zone, the tower", 6,
       {"M6_BZ_Start", "M6_BZ_FirstMindControl", "M6_BZ_SecondMindControl", "M6_BZ_FightPyro", "M6_BZ_GambitFight",
        "M6_BZ_ForgeIntro", "M6_BZ_FinalBattle", "M6_BZ_SatelliteTower"}},
      {"Mission 7 - SoMa", 7,
       {"M7_Soma_Rooftop_1", "M7_Soma_Rooftop_2", "M7_Soma_BuildingClimb", "M7_Soma_SentinelFight",
        "M7_Soma_BastionFight"}},
  };
  return missions;
}

const char* const kArenas[] = {
    "1_M1_FB_Sewers_Arena",     "2_M2_FB_Pyro_Arena",        "3_M3_CT_Mystique_Arena",   "4_M3_CT_MarketPlace_Arena",
    "5_M3_CT_Sewers_Arena",     "6_M3_CT_Iceman_Arena",      "7_M3_Lab_DegenRoom_Arena", "8_M3_Lab_Surge_Arena",
    "9_M3_Lab_Quicksilver_Arena", "10_M4_Fissure1_Arena",    "11_M4_Fissure2_Arena",     "12_M4_Fissure3_Arena",
    "13_M5_BZ_Northstar_Arena", "14_M5_BZ_Juggernaut_Arena", "15_M6_BZ_Mech_Arena"};

struct Scene {
  const char* archive;  // (the level script that starts it: the entry's group)
  int number;
  const char* actor;
  const char* own;      // (the scene's own archive, "" when the script names none)
  const char* packs;    // (character packs it uses, space-separated)
  const char* checkpoint;  // (where it was seen to play: loaded first)
  int mission;
};
const Scene kScenes[] = {
#include "mod_menu_tables.inc"
};

// The powers the script interface can switch on and level: Enable<name>(1), Level<name>(n).
const char* const kPowers[] = {"Aftershock",  "BoulderDash",  "CriticalMass", "DarkVapor",   "EdgeStorm",
                               "EnergyOrb",   "EverSource",   "ForceField",   "FortifiedBarricade", "GigaBeam",
                               "GroundSmash", "Hover",        "InfinityRift", "IonShot",     "LifeSteal",
                               "MeteorDive",  "ObsidianShards", "ObsidianSkin", "ScatterBeam", "ShadowGrasp",
                               "ShadowPhase", "Titan",        "Tremor",       "VoidWalk"};

void SetSpeed(float speed) {
  g_speed = speed;
  char text[64];
  std::snprintf(text, sizeof(text), "settimedialation(%.3f)", speed);
  Lua(text);
  std::snprintf(text, sizeof(text), "Game speed %.2fx", speed);
  ShowNotice(text, 2.0f);
}

void StartScene(const Scene& scene) {
  // (what the scene needs first - loading something again does no harm - then its actor once that is there)
  std::string lua;
  std::istringstream packs(scene.packs);
  for (std::string pack; packs >> pack;) lua += "load(\"" + pack + "\") ";
  if (scene.own[0]) lua += std::string("load(\"") + scene.own + "\") ";
  else lua += std::string("load(\"") + scene.archive + "\") ";
  Lua(lua);
  const std::string actor = scene.actor;
  Later(7000, [actor] { Lua("cineplexaction(\"" + actor + "\", 0)"); });
  ShowNotice(std::string("Cut-scene: ") + (scene.own[0] ? scene.own : scene.archive) + " - loading ...", 6.0f);
}

// The scene's place first (the checkpoint tools\mod_sweep.ps1 saw it play from), then the scene.
void PlayScene(const Scene& scene) {
  if (!scene.checkpoint[0]) return StartScene(scene);
  ShowNotice(std::string("Cut-scene: loading ") + scene.checkpoint + " first ...", 5.0f);
  Warp(scene.checkpoint, scene.mission, false, [scene] { Later(3000, [scene] { StartScene(scene); }); });
}

// The player moved by an offset (the game's units: 100 are about a metre).
void Nudge(float dx, float dy, float dz) {
  debug::QueryPosition([dx, dy, dz](bool ok, const std::string& text) {
    float x, y, z;
    if (!ok || std::sscanf(text.c_str(), "%f %f %f", &x, &y, &z) != 3) return;
    debug::GotoPosition(x + dx, y + dy, z + dz, [](bool, const std::string& now) {
      REXLOG_INFO("mod menu: moved to {}", now);
    });
  });
}

void Build() {
  const int root = AddPage("Mod menu");

  // -- cheats
  const int cheats = AddPage("Cheats");
  Switch(cheats, "god", "God mode", "Health is refilled four times a second.", &g_god, nullptr);
  Switch(cheats, "power", "Infinite power energy", "The power bar is refilled four times a second.", &g_power,
         nullptr);
  Action(cheats, "heal", "Refill health", "", [] { Lua("RestoreMainPlayerHealth()"); });
  Action(cheats, "refill_power", "Refill power energy", "", [] { Lua("RestoreMPower()"); });
  Action(cheats, "xp", "Add 5,000 XP", "Experience to spend on powers in the pause menu.",
         [] { Lua("AddXP(5000, \"\")"); });
  Action(cheats, "xp_big", "Add 100,000 XP", "", [] { Lua("AddXP(100000, \"\")"); });
  Action(cheats, "xgenes", "Unlock all X-genes and suits", "Every X-gene is put in the inventory (pause menu).",
         [] { Lua("givemutator() giveallmutators(\"all\")"); });
  Action(cheats, "powers", "Unlock and max all powers", "Every power of the script interface switched on at level 3.",
         [] {
           std::string lua;
           for (const char* p : kPowers) {
             lua += std::string("pcall(Enable") + p + ",1) pcall(Level" + p + ",3) ";
           }
           Lua(lua);
         });
  Switch(cheats, "freeze_enemies", "Freeze enemies", "Enemies stop thinking (the game's DisableAI).",
         &g_enemies_frozen, [](bool on) { Lua(on ? "DisableAI()" : "EnableAI()"); });
  Switch(cheats, "hide_hud", "Hide the HUD", "", &g_hud_hidden,
         [](bool on) { Lua(on ? "showhud(false)" : "showhud(true)"); });
  Switch(cheats, "hide_player", "Hide the player", "", &g_player_hidden,
         [](bool on) { Lua(on ? "hidemainplayer(true)" : "hidemainplayer(false)"); });
  Action(cheats, "control", "Give control back", "After a cut-scene that left the player stuck.",
         [] { Lua("EnablePlayerControl() showhud(true)"); });

  // -- game speed
  const int speed = AddPage("Game speed");
  for (float s : {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f}) {
    char label[32], id[32];
    std::snprintf(label, sizeof(label), "%.2fx%s", s, s == 1.0f ? " (normal)" : "");
    std::snprintf(id, sizeof(id), "speed_%d", int(s * 100));
    Action(speed, id, label, "", [s] { SetSpeed(s); });
  }
  Action(speed, "freeze", "Freeze / unfreeze the world (F5)", "", [] {
    const bool frozen = !debug::Frozen();
    debug::SetFrozen(frozen);
    ShowNotice(frozen ? "Frozen" : "Running", 2.0f);
  });

  // -- teleport
  const int teleport = AddPage("Teleport");
  for (const Mission& m : Missions()) {
    const int page = AddPage(m.title);
    for (size_t i = 0; i < m.checkpoints.size(); ++i) {
      const std::string name = m.checkpoints[i];
      const int number = m.number;
      Action(page, "cp_" + Slug(name), i == 0 ? "Start of the mission" : Pretty(name), name,
             [name, number] {
               ShowNotice("Loading checkpoint " + name + " ...", 4.0f);
               Warp(name, number);
             });
    }
    Link(teleport, page, "missions_" + std::to_string(m.number));
  }
  const int arenas = AddPage("Challenge arenas");
  for (const char* a : kArenas) {
    const std::string name = a;
    Action(arenas, "cp_" + Slug(name), Pretty(name), name, [name] {
      ShowNotice("Loading " + name + " ...", 4.0f);
      Warp(name, -1);
    });
  }
  Link(teleport, arenas, "arenas");
  const int spots = AddPage("Saved spots (this level)");
  for (int n = 1; n <= kSpots; ++n) {
    Action(spots, "spot_go_" + std::to_string(n), "Go to spot " + std::to_string(n), "", [n] { GotoSpot(n); });
  }
  for (int n = 1; n <= kSpots; ++n) {
    Action(spots, "spot_save_" + std::to_string(n), "Save this place as spot " + std::to_string(n), "",
           [n] { SaveSpot(n); });
  }
  Link(teleport, spots, "spots", "Places remembered by their position: valid in the level they were saved in.");
  Action(teleport, "reload", "Reload the last checkpoint", "", [] { Lua("LoadCheckpoint()"); });
  const int nudge = AddPage("Move the player");
  Action(nudge, "move_up", "Up 5 m", "", [] { Nudge(0, 0, 500); });
  Action(nudge, "move_down", "Down 5 m", "", [] { Nudge(0, 0, -500); });
  Action(nudge, "move_xp", "+X 5 m", "", [] { Nudge(500, 0, 0); });
  Action(nudge, "move_xm", "-X 5 m", "", [] { Nudge(-500, 0, 0); });
  Action(nudge, "move_yp", "+Y 5 m", "", [] { Nudge(0, 500, 0); });
  Action(nudge, "move_ym", "-Y 5 m", "", [] { Nudge(0, -500, 0); });
  Link(teleport, nudge, "move", "Through walls and floors, five metres at a time.");

  // -- cut-scenes
  const int scenes = AddPage("Cut-scenes");
  {
    int page = -1;
    std::string archive;
    for (const Scene& s : kScenes) {
      if (archive != s.archive) {
        archive = s.archive;
        page = AddPage(Pretty(archive));
        Link(scenes, page, "scenes_" + Slug(archive), archive);
      }
      const Scene scene = s;
      Action(page, "scene_" + Slug(archive) + "_" + std::to_string(s.number),
             "Scene " + std::to_string(s.number) + (s.own[0] ? "  (" + Pretty(s.own) + ")" : std::string()),
             std::string("Loads ") + s.checkpoint + " and plays the scene there. While it plays the game "
             "takes nothing from this menu.",
             [scene] { PlayScene(scene); });
    }
  }

  // -- character
  const int character = AddPage("Character and suits");
  Action(character, "suits", "Unlock every suit and X-gene",
         "Then wear one in the game's pause menu: X-Genes & Suits > Suits (Wolverine, Cyclops, Iceman, Surge, "
         "Colossus, Emma Frost, Gambit, Juggernaut, Magneto ...). The suits are the game's own way to play in "
         "another character's look.",
         [] {
           Lua("givemutator() giveallmutators(\"all\")");
           ShowNotice("All suits and X-genes unlocked - Pause > X-Genes & Suits", 4.0f);
         });
  Action(character, "powers2", "Unlock and max all powers", "", [] {
    if (Entry* e = Find("powers")) e->run();
  });

  Link(root, cheats, "cheats");
  Link(root, teleport, "teleport");
  Link(root, scenes, "cutscenes", "In-engine scenes. Best played in the level they belong to.");
  Link(root, character, "character");
  Link(root, speed, "speed");
}

// ---- the overlay -----------------------------------------------------------------------------------------------

enum class Step { kNone, kUp, kDown, kOk, kBack, kPageUp, kPageDown };

void Apply(Step step) {
  std::lock_guard lock(g_ui_mutex);
  Page& page = g_pages[g_stack.back()];
  const int count = int(page.entries.size());
  switch (step) {
    case Step::kUp:
      if (count) page.selected = (page.selected + count - 1) % count;
      break;
    case Step::kDown:
      if (count) page.selected = (page.selected + 1) % count;
      break;
    case Step::kPageUp:
      page.selected = std::max(0, page.selected - 8);
      break;
    case Step::kPageDown:
      page.selected = std::min(count - 1, page.selected + 8);
      break;
    case Step::kBack:
      if (g_stack.size() > 1) g_stack.pop_back();
      else g_open = false;
      break;
    case Step::kOk:
      if (page.selected >= 0 && page.selected < count) {
        const Entry& e = page.entries[page.selected];
        if (e.page >= 0) {
          g_stack.push_back(e.page);
        } else if (e.run) {
          REXLOG_INFO("mod menu: {} ({})", e.label, e.id);
          e.run();
        }
      }
      break;
    default:
      break;
  }
}

class Menu : public rex::ui::ImGuiDialog {
 public:
  explicit Menu(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint16_t pad_before_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{};
  uint16_t repeat_button_ = 0;
};

Menu* g_menu = nullptr;

void Menu::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  // The controller: as the game got it while the menu is closed, the held-back state while it is open.
  uint16_t pad = 0;
  int16_t stick_y = 0;
  if (g_open) {
    X_INPUT_STATE state{};
    if (g_input && g_input->HeldState(0, &state)) {
      pad = state.gamepad.buttons;
      stick_y = state.gamepad.thumb_ly;
    }
  } else {
    pad = g_pad_seen.load();
  }
  if (stick_y > 20000) pad |= X_INPUT_GAMEPAD_DPAD_UP;
  if (stick_y < -20000) pad |= X_INPUT_GAMEPAD_DPAD_DOWN;
  const uint16_t both = X_INPUT_GAMEPAD_LEFT_THUMB | X_INPUT_GAMEPAD_RIGHT_THUMB;
  const uint16_t pressed = pad & ~pad_before_;
  if ((pad & both) == both && (pad_before_ & both) != both) {
    g_open = !g_open.load();
    wait_release_ = true;
    REXLOG_INFO("mod menu: {} (controller)", g_open ? "opened" : "closed");
  }
  pad_before_ = pad;
  if (!g_open) return;
  if (wait_release_ && pad == 0) wait_release_ = false;

  std::vector<Step> steps;
  if (!wait_release_) {
    const auto now = Clock::now();
    const auto held = [&](uint16_t button) {
      // (pressed now, or held: again after 400 ms, then every 90 ms)
      if (pressed & button) {
        repeat_button_ = button;
        repeat_at_ = now + std::chrono::milliseconds(400);
        return true;
      }
      if ((pad & button) && repeat_button_ == button && now >= repeat_at_) {
        repeat_at_ = now + std::chrono::milliseconds(90);
        return true;
      }
      return false;
    };
    if (held(X_INPUT_GAMEPAD_DPAD_UP)) steps.push_back(Step::kUp);
    if (held(X_INPUT_GAMEPAD_DPAD_DOWN)) steps.push_back(Step::kDown);
    if (pressed & X_INPUT_GAMEPAD_A) steps.push_back(Step::kOk);
    if (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_BACK)) steps.push_back(Step::kBack);
    if (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) steps.push_back(Step::kPageUp);
    if (pressed & X_INPUT_GAMEPAD_RIGHT_SHOULDER) steps.push_back(Step::kPageDown);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) steps.push_back(Step::kUp);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) steps.push_back(Step::kDown);
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) steps.push_back(Step::kPageUp);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) steps.push_back(Step::kPageDown);
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))
    steps.push_back(Step::kOk);
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    steps.push_back(Step::kBack);
  {
    std::lock_guard lock(g_ui_mutex);
    for (const std::string& key : g_keys) {
      steps.push_back(key == "up" ? Step::kUp : key == "down" ? Step::kDown : key == "ok" || key == "right"
                          ? Step::kOk : key == "back" || key == "left" ? Step::kBack : key == "pageup"
                          ? Step::kPageUp : key == "pagedown" ? Step::kPageDown : Step::kNone);
    }
    g_keys.clear();
  }

  // The list (drawn from a copy of what it needs: entries run outside the lock).
  int clicked = -1, hovered = -1;
  const bool keyboard_or_pad_moved = !steps.empty();
  {
    std::lock_guard lock(g_ui_mutex);
    const Page& page = g_pages[g_stack.back()];
    const float k = std::max(1.0f, io.DisplaySize.y / 720.0f);
    const float width = 500.0f * k;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.06f, io.DisplaySize.y * 0.14f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y * 0.72f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##xmd_mod_menu", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.55f * k);
      ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f), "X-MEN DESTINY  -  MOD MENU");
      ImGui::SetWindowFontScale(1.2f * k);
      std::string crumbs;
      for (size_t i = 0; i < g_stack.size(); ++i) {
        crumbs += (i ? "  >  " : "") + g_pages[g_stack[i]].title;
      }
      ImGui::TextDisabled("%s", crumbs.c_str());
      ImGui::Separator();
      const float footer = ImGui::GetTextLineHeightWithSpacing() * 4.6f;
      if (ImGui::BeginChild("##entries", ImVec2(0, -footer), false, ImGuiWindowFlags_NoNav)) {
        ImGui::SetWindowFontScale(1.3f * k);
        for (int i = 0; i < int(page.entries.size()); ++i) {
          const Entry& e = page.entries[i];
          std::string label = e.label;
          if (e.page >= 0) label += "   >";
          if (e.on) label = std::string(e.on() ? "[x] " : "[ ] ") + label;
          const bool selected = i == page.selected;
          if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.4f, 1.0f));
          if (ImGui::Selectable((label + "##" + std::to_string(i)).c_str(), selected)) clicked = i;
          if (selected) ImGui::PopStyleColor();
          if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) hovered = i;
          if (selected && keyboard_or_pad_moved) ImGui::SetScrollHereY(0.5f);
        }
      }
      ImGui::EndChild();
      ImGui::Separator();
      ImGui::SetWindowFontScale(1.05f * k);
      ImGui::PushTextWrapPos(width - 12.0f * k);
      const std::string note = page.selected >= 0 && page.selected < int(page.entries.size())
                                   ? page.entries[page.selected].note : std::string();
      ImGui::TextUnformatted(note.empty() ? " " : note.c_str());
      ImGui::TextDisabled("Up / Down: choose    Enter or A: select    Backspace or B: back\n"
                          "F1 or both sticks pressed: close    Mouse: click an entry");
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }
  if (hovered >= 0 || clicked >= 0) {
    std::lock_guard lock(g_ui_mutex);
    g_pages[g_stack.back()].selected = clicked >= 0 ? clicked : hovered;
  }
  for (Step step : steps) Apply(step);
  if (clicked >= 0) Apply(Step::kOk);
}

}  // namespace

bool IsOpen() { return g_open; }
uint32_t ScratchSize() { return kScratchSize; }

void Install(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_data) {
  g_user_data = user_data;
  Build();
  LoadSpots();
  g_menu = new Menu(drawer);  // (lives for the whole run)
  rex::ui::RegisterBind("bind_mod_menu", "F1",
                        "Mod menu: cheats, teleports, cut-scenes, suits, game speed (controller: both sticks pressed)",
                        [] {
                          g_open = !g_open.load();
                          REXLOG_INFO("mod menu: {}", g_open ? "opened" : "closed");
                        });
  std::thread(Ticker).detach();
}

void AttachGame(rex::input::InputSystem* input, uint32_t scratch) {
  g_scratch = scratch;
  g_input = input;
  if (!input) return;
  input->SetGuestInputHold([] { return g_open.load(std::memory_order_relaxed); });
  // (a recording session - tools\record_play.ps1 - has the observer: no controller shortcut then)
  if (!std::getenv("XMD_RECORD_FILE")) {
    input->SetStateObserver([](uint32_t user, const rex::input::X_INPUT_STATE& state) {
      if (user == 0) g_pad_seen.store(state.gamepad.buttons, std::memory_order_relaxed);
    });
  }
}

bool Command(const std::string& verb, const std::string& rest) {
  if (verb != "mod") return false;
  std::istringstream in(rest);
  std::string what, arg;
  in >> what;
  std::getline(in, arg);
  if (const size_t first = arg.find_first_not_of(" \t"); first != std::string::npos) arg.erase(0, first);
  else arg.clear();
  if (what == "open" || what == "close") {
    g_open = what == "open";
    REXLOG_INFO("mod menu: {}", g_open ? "opened" : "closed");
  } else if (what == "key") {
    std::lock_guard lock(g_ui_mutex);
    g_keys.push_back(arg);
  } else if (what == "list") {
    for (const Page& page : g_pages) {
      for (const Entry& e : page.entries) {
        REXLOG_INFO("mod menu: entry {:<40} {} / {}", e.id, page.title, e.label);
      }
    }
  } else if (what == "run") {
    if (Entry* e = Find(arg)) {
      REXLOG_INFO("mod menu: run {} ({})", e->id, e->label);
      if (e->run) e->run();
      else if (e->page >= 0) {
        std::lock_guard lock(g_ui_mutex);
        g_stack.push_back(e->page);
      }
    } else {
      REXLOG_WARN("mod menu: no entry '{}' (mod list shows them)", arg);
    }
  } else if (what == "checkpoint") {
    std::istringstream a(arg);
    std::string name;
    int mission = -1;
    a >> name >> mission;
    Warp(name, mission);
  } else if (what == "peek" || what == "poke") {
    // (the game object's fields, for finding out what a function left behind: mod peek 7FC / mod poke 7FC 1)
    uint32_t offset = 0, value = 0;
    const int got = std::sscanf(arg.c_str(), "%x %x", &offset, &value);
    const bool poke = what == "poke" && got == 2;
    if (got >= 1 && offset < 0x2000) {
      debug::RunNative([offset, value, poke](PPCContext&, uint8_t* base) {
        const uint32_t game = Load32(base, kGame);
        if (game < 0x10000) return;
        const uint32_t before = Load32(base, game + offset);
        if (poke) Store32(base, game + offset, value);
        REXLOG_INFO("mod menu: game + {:X}: {:08X}{}", offset, before, poke ? fmt::format(" -> {:08X}", value) : "");
      });
    }
  } else if (what == "lua") {
    Lua(arg);
  } else {
    REXLOG_WARN("mod menu: unknown '{}' (open, close, key, list, run, checkpoint, lua)", what);
  }
  return true;
}

}  // namespace xmd::modmenu
