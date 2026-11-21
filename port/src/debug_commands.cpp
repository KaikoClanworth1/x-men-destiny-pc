// X-Men: Destiny - debug commands: a test puts the player where it needs them.
//
// The game's missions are Lua scripts (text in the level archives; tools\lua_scripts.py writes them to
// runs\lua\) calling the engine: Teleport(actors.., target, bool, bool), TriggerNamedEvent(name, players..),
// SetGameFlag(name), ActivateActorSpawnNode(nodes..), LoadCheckpoint() ... Actors are named by their ID:
// SetLuaVariableToActorUID("Player1"), SetLuaVariableToActorUID("d8f2dd55-76c0-4e09-ab1d-ea8303408f1f").
//
// With XMD_DEBUG_FILE=<file> set (tools\run_test.ps1: runs\debug.txt), lines appended to that file are run as
// Lua in the game's own script state, on the thread that runs the game's scripts (at its next call into Lua):
//
//   lua <code>               any Lua, one line
//   luafile <path>           a file of Lua
//   teleport <actor id>      the player to that actor (a spawn node, a marker, a character)
//   spawn <node id> [...]    ActivateActorSpawnNode on those nodes
//   event <name>             TriggerNamedEvent(name, players)
//   flag <name>              SetGameFlag(name)
//   capture                  what F4 does (capture.h)
//   where                    the player's position, in the log
//   goto <x> <y> <z>         the player to that position (the numbers "where" and a capture's info.txt give)
//   freeze / unfreeze        the world held still, frames still drawn (F5; SetFrozen)
//
// The result of each is a log line: "debug command: ok ..." or "debug command: error: <Lua's message>".
//
//   sub_826A03D0  lua_pcall(L, nargs, nresults, errfunc)
//   sub_826A61D0  lua_resume(L, nargs)
//   sub_826A04B0  lua_load(L, reader, data, chunkname)     reader 0x826B7788 (lauxlib's getS: data = {text, size})
//   sub_8269FC78  lua_tostring(L, index)
//   (the engine's own "run this text": sub_825D9EC0)

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "capture.h"
#include "debug_commands.h"
#include "controls.h"
#include "mod_menu.h"
#include "generated/default/xmd_init.h"

REX_EXTERN(__imp__sub_826A03D0);
REX_EXTERN(__imp__sub_826A61D0);
REX_EXTERN(sub_826A04B0);
REX_EXTERN(sub_8269FC78);
REX_EXTERN(sub_82661F68);
REX_EXTERN(sub_8233D5D8);

namespace xmd::debug {

namespace {

constexpr uint32_t kReader = 0x826B7788;
constexpr uint32_t kScratchSize = 0x10000;

std::mutex g_mutex;
struct Pending {
  std::string lua;
  Answer answer;  // (Query: gets the chunk's value)
  enum Kind { kLua, kWhere, kGoto, kNative } kind = kLua;
  float x = 0, y = 0, z = 0;  // (kGoto)
  Native native;              // (kNative: the mod menu's own work on the script thread)
};
std::deque<Pending> g_pending;
std::atomic<bool> g_any{false};
uint32_t g_scratch = 0;  // (guest memory for the text handed to lua_load)

const char kPlayers[] =
    "local P1,P2,P3,P4 = SetLuaVariableToActorUID(\"Player1\"),SetLuaVariableToActorUID(\"Player2\"),"
    "SetLuaVariableToActorUID(\"Player3\"),SetLuaVariableToActorUID(\"Player4\") ";

void Queue(std::string lua, Answer answer = {}, Pending::Kind kind = Pending::kLua, float x = 0, float y = 0,
           float z = 0) {
  std::lock_guard lock(g_mutex);
  g_pending.push_back(Pending{std::move(lua), std::move(answer), kind, x, y, z});
  g_any = true;
}

void ParseLine(const std::string& line) {
  std::istringstream in(line);
  std::string verb;
  if (!(in >> verb) || verb[0] == '#') return;
  std::string rest;
  std::getline(in, rest);
  if (const size_t first = rest.find_first_not_of(" \t"); first != std::string::npos) rest.erase(0, first);
  while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
  if (verb == "lua") {
    Queue(rest);
  } else if (verb == "luafile") {
    std::ifstream f(rest, std::ios::binary);
    std::stringstream text;
    text << f.rdbuf();
    if (!f) {
      REXLOG_WARN("debug command: cannot read {}", rest);
      return;
    }
    Queue(text.str());
  } else if (verb == "teleport") {
    Queue(std::string(kPlayers) + "Teleport(P1,P2,P3,P4,SetLuaVariableToActorUID(\"" + rest + "\"),true,false)");
  } else if (verb == "spawn") {
    std::istringstream ids(rest);
    std::string lua = "ActivateActorSpawnNode(";
    bool first = true;
    for (std::string id; ids >> id; first = false) {
      lua += (first ? "" : ",") + ("SetLuaVariableToActorUID(\"" + id + "\")");
    }
    Queue(lua + ")");
  } else if (verb == "event") {
    Queue(std::string(kPlayers) + "TriggerNamedEvent(\"" + rest + "\",P1,P2,P3,P4)");
  } else if (verb == "where") {
    Queue("where", [](bool ok, const std::string& text) {
      REXLOG_INFO("debug command: where: {}{}", ok ? "" : "not known: ", text);
    }, Pending::kWhere);
  } else if (verb == "goto") {
    float x = 0, y = 0, z = 0;
    std::istringstream at(rest);
    at >> x >> y >> z;
    Queue("goto " + rest, [](bool ok, const std::string& text) {
      REXLOG_INFO("debug command: goto: {}{}", ok ? "now at " : "failed: ", text);
    }, Pending::kGoto, x, y, z);
  } else if (verb == "capture") {
    xmd::TakeCapture();  // (what F4 does: capture.h)
  } else if (verb == "freeze" || verb == "unfreeze") {
    SetFrozen(verb == "freeze");
  } else if (verb == "set") {  // set <setting> <value>: a runtime setting (rex::cvar), as the Graphics menu sets one
    std::istringstream parts(rest);
    std::string name, value;
    parts >> name >> value;
    const bool ok = !name.empty() && rex::cvar::GetFlagInfo(name) && rex::cvar::SetFlagByName(name, value);
    REXLOG_INFO("debug command: set {} = {}{}", name, value, ok ? "" : " (no such setting, or not that value)");
  } else if (verb == "flag") {
    Queue("SetGameFlag(\"" + rest + "\")");
  } else if (xmd::controls::Command(verb, rest)) {  // ("controls ...", "cam ...": controls.h)
  } else if (!xmd::modmenu::Command(verb, rest)) {  // ("mod ...": the mod menu's functions, mod_menu.h)
    REXLOG_WARN("debug command: unknown '{}'", line);
  }
}

void Poll(const std::string& file) {
  size_t consumed = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::ifstream f(file, std::ios::binary);
    if (!f) continue;
    f.seekg(0, std::ios::end);
    const size_t size = static_cast<size_t>(f.tellg());
    if (size < consumed) consumed = 0;
    if (size == consumed) continue;
    f.seekg(static_cast<std::streamoff>(consumed));
    std::string text(size - consumed, '\0');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    const size_t end = text.rfind('\n');
    if (end == std::string::npos) continue;
    consumed += end + 1;
    std::istringstream lines(text.substr(0, end));
    for (std::string line; std::getline(lines, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      REXLOG_INFO("debug command: {}", line.substr(0, 200));
      ParseLine(line);
    }
  }
}

// The player and where they stand, the way the game's own console command "moveplayer" (sub_825474F0) finds them:
// the game object at [0x8366855C]; its [+1364] (which player) and [+252] handed to sub_82661F68, the result to
// sub_8233D5D8 -> the player; [player + 824] is its body, with the position at +540 (three floats) and a
// "set position" in its vtable at +528.
constexpr uint32_t kGameObject = 0x8366855C;

uint32_t Guest32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return _byteswap_ulong(v);
}
float GuestFloat(uint8_t* base, uint32_t address) {
  const uint32_t v = Guest32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void PutGuestFloat(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = _byteswap_ulong(v);
  std::memcpy(base + address, &v, 4);
}

// The player's body, or 0 (no game object yet, no player - menus, loading).
uint32_t PlayerBody(PPCContext& ctx, uint8_t* base) {
  const uint32_t game = Guest32(base, kGameObject);
  if (game < 0x10000) return 0;
  const uint32_t which = Guest32(base, game + 1364);
  if (!which) return 0;
  ctx.r3.u64 = Guest32(base, game + 252);
  ctx.r4.u64 = which;
  sub_82661F68(ctx, base);
  sub_8233D5D8(ctx, base);
  const uint32_t player = ctx.r3.u32;
  if (player < 0x10000) return 0;
  const uint32_t body = Guest32(base, player + 824);
  return body >= 0x10000 ? body : 0;
}

void RunPosition(PPCContext& ctx, uint8_t* base, const Pending& item) {
  const PPCContext saved = ctx;
  ctx.r1.u32 -= 512;
  const uint32_t body = PlayerBody(ctx, base);
  if (!body) {
    ctx = saved;
    if (item.answer) item.answer(false, "no player in the world");
    return;
  }
  if (item.kind == Pending::kGoto && g_scratch) {
    PutGuestFloat(base, g_scratch + 0, item.x);
    PutGuestFloat(base, g_scratch + 4, item.y);
    PutGuestFloat(base, g_scratch + 8, item.z);
    const uint32_t set_position = Guest32(base, Guest32(base, body) + 528);
    ctx.r3.u64 = body;
    ctx.r4.u64 = g_scratch;
    ctx.ctr.u64 = set_position;
    REX_CALL_INDIRECT_FUNC(set_position);
  }
  char text[160];
  std::snprintf(text, sizeof(text), "%.3f %.3f %.3f", GuestFloat(base, body + 540), GuestFloat(base, body + 544),
                GuestFloat(base, body + 548));
  ctx = saved;
  if (item.answer) item.answer(true, text);
}

// Runs one piece of Lua text in `state`, the way the engine's own sub_825D9EC0 does.
void Run(PPCContext& ctx, uint8_t* base, uint32_t state, const Pending& item) {
  if (item.kind == Pending::kNative) {
    const PPCContext saved = ctx;
    ctx.r1.u32 -= 512;
    if (item.native) item.native(ctx, base);
    ctx = saved;
    return;
  }
  if (item.kind != Pending::kLua) {
    RunPosition(ctx, base, item);
    return;
  }
  const std::string& lua = item.lua;
  if (!g_scratch || lua.size() + 64 > kScratchSize) {
    REXLOG_WARN("debug command: no room for {} bytes of Lua", lua.size());
    if (item.answer) item.answer(false, "too long");
    return;
  }
  // [0] text pointer, [4] size, [8] the chunk's name, [32] the text
  uint8_t* scratch = base + g_scratch;
  const uint32_t text = g_scratch + 32;
  std::memcpy(scratch + 32, lua.data(), lua.size());
  const uint32_t be_text = _byteswap_ulong(text), be_size = _byteswap_ulong(static_cast<uint32_t>(lua.size()));
  std::memcpy(scratch, &be_text, 4);
  std::memcpy(scratch + 4, &be_size, 4);
  std::memcpy(scratch + 8, "=debug", 7);

  const PPCContext saved = ctx;
  ctx.r1.u32 -= 512;  // (clear of whatever the caller keeps just under its stack pointer)
  ctx.r3.u64 = state;
  ctx.r4.u64 = kReader;
  ctx.r5.u64 = g_scratch;
  ctx.r6.u64 = g_scratch + 8;
  sub_826A04B0(ctx, base);
  uint32_t status = ctx.r3.u32;
  const char* stage = "does not parse";
  if (status == 0) {
    ctx.r3.u64 = state;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = item.answer ? 1 : 0;  // (results: a Query's one value)
    ctx.r6.u64 = 0;
    __imp__sub_826A03D0(ctx, base);
    status = ctx.r3.u32;
    stage = "failed";
  }
  // (a value off the script stack: 8 bytes each, the top at state + 8)
  const auto top_text_and_pop = [&] {
    ctx.r3.u64 = state;
    ctx.r4.s64 = -1;
    sub_8269FC78(ctx, base);
    const uint32_t message = ctx.r3.u32;
    std::string what = "(no text)";
    if (message >= 0x10000) what = reinterpret_cast<const char*>(base + message);
    uint32_t top;
    std::memcpy(&top, base + state + 8, 4);
    top = _byteswap_ulong(_byteswap_ulong(top) - 8);
    std::memcpy(base + state + 8, &top, 4);
    return what;
  };
  if (status == 0 && item.answer) {
    item.answer(true, top_text_and_pop());
  } else if (status == 0) {
    REXLOG_INFO("debug command: ok: {}", lua.substr(0, 160));
  } else if (item.answer) {
    item.answer(false, top_text_and_pop());
  } else {
    REXLOG_WARN("debug command: error: {} ({}): {}", stage, top_text_and_pop().substr(0, 300), lua.substr(0, 160));
  }
  ctx = saved;
}

}  // namespace

void Install(const std::string& file, uint32_t scratch) {
  g_scratch = scratch;
  if (file.empty()) return;
  REXLOG_INFO("debug commands: reading {}", file);
  std::thread(Poll, file).detach();
}

void Query(std::string lua, Answer answer) { Queue(std::move(lua), std::move(answer)); }

// The game's own time scale (its console's settimedialation): at 0.001 nothing moves while every frame is still
// drawn - the scene can be looked at, captured, stepped through. 0 stops the game loop itself (it hangs).
std::atomic<bool> g_frozen{false};
void SetFrozen(bool frozen) {
  g_frozen = frozen;
  Queue(frozen ? "settimedialation(0.001)" : "settimedialation(1)");
}
bool Frozen() { return g_frozen; }
void QueryPosition(Answer answer) { Queue("where", std::move(answer), Pending::kWhere); }
void GotoPosition(float x, float y, float z, Answer answer) {
  Queue("goto", std::move(answer), Pending::kGoto, x, y, z);
}
void RunLua(std::string lua) { Queue(std::move(lua)); }
void RunNative(Native work) {
  std::lock_guard lock(g_mutex);
  Pending item;
  item.kind = Pending::kNative;
  item.native = std::move(work);
  g_pending.push_back(std::move(item));
  g_any = true;
}

uint32_t ScratchSize() { return kScratchSize; }
bool Waiting() { return g_any.load(std::memory_order_relaxed); }

}  // namespace xmd::debug

namespace {

std::atomic<uint32_t> g_main_state{0};  // (the main script state, once the game has made its first call)

void RunPending(PPCContext& ctx, uint8_t* base, uint32_t state) {
  using namespace xmd::debug;
  static thread_local bool inside = false;
  if (!g_any || inside || state < 0x10000) return;
  inside = true;
  std::deque<Pending> run;
  {
    std::lock_guard lock(g_mutex);
    run.swap(g_pending);
    g_any = false;
  }
  {
    // (one thread at a time: the commands' text goes through one piece of guest memory, and during a level load
    // the loader's thread calls into the scripts as well as the game's - two texts mixed there were a script
    // syntax error and a crash, 2026-11-20)
    static std::mutex one_at_a_time;
    std::lock_guard lock(one_at_a_time);
    for (const Pending& item : run) Run(ctx, base, state, item);
  }
  inside = false;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return _byteswap_ulong(v);
}

}  // namespace

// lua_pcall: a call into the scripts from the engine. What is waiting runs first, in the same script state.
REX_HOOK_RAW(sub_826A03D0) {
  RunPending(ctx, base, ctx.r3.u32);
  __imp__sub_826A03D0(ctx, base);
}

// lua_resume(L, nargs): the mission scripts are coroutines the engine goes on with every frame (LuaWaitEvent
// yields). What is waiting runs first - in the main script state, not the coroutine's, which is suspended in
// the middle of a function: the main one is at L->l_G (+16) ->mainthread (+60).
REX_HOOK_RAW(sub_826A61D0) {
  const uint32_t global = Load32(base, ctx.r3.u32 + 16);
  if (global >= 0x10000) RunPending(ctx, base, Load32(base, global + 60));
  __imp__sub_826A61D0(ctx, base);
}

// luaD_call(L, func, nresults): every call of a function from C goes through it, the engine's calls into its
// scripts among them (the scheduler that goes on with the mission scripts is itself Lua). At a call from
// outside any script (L->nCcalls, +46, still 0), what is waiting runs first, in the main script state.
REX_EXTERN(__imp__sub_826A5F58);
REX_HOOK_RAW(sub_826A5F58) {
  if (!g_main_state.load(std::memory_order_relaxed)) {
    const uint32_t global = Load32(base, ctx.r3.u32 + 16);
    if (global >= 0x10000) g_main_state = Load32(base, global + 60);
  }
  if (xmd::debug::Waiting()) {
    uint16_t nested;
    std::memcpy(&nested, base + ctx.r3.u32 + 46, 2);
    const uint32_t global = Load32(base, ctx.r3.u32 + 16);
    if (nested == 0 && global >= 0x10000) RunPending(ctx, base, Load32(base, global + 60));
  }
  __imp__sub_826A5F58(ctx, base);
}

// The game thread's way down to a mission script (a crash's stack, 2026-11-18): sub_8299D350 (thread start) ->
// sub_82661170 -> sub_82666A10 -> sub_8265BEE8 -> sub_82527990 -> sub_8265D160 -> sub_826605D8 -> sub_825D9538
// -> sub_825D9488 -> sub_82550170 (loads and runs a script's text). A player standing still calls no script,
// so what is waiting also runs at a place on that way that comes round every frame: XMD_DEBUG_TICK=<n> picks
// it (default below), XMD_DEBUG_TICK_COUNT=1 logs how often each is called.
namespace {

std::atomic<uint64_t> g_tick_calls[5];
const int g_tick_choice = [] {
  const char* v = std::getenv("XMD_DEBUG_TICK");
  return v ? std::atoi(v) : 0;
}();
const bool g_tick_count = std::getenv("XMD_DEBUG_TICK_COUNT") != nullptr;

void Tick(int which, PPCContext& ctx, uint8_t* base) {
  if (g_tick_count) {
    static std::atomic<int64_t> last{0};
    ++g_tick_calls[which];
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t before = last.load();
    if (now - before >= 5 && last.compare_exchange_strong(before, now)) {
      REXLOG_INFO("debug command: calls in 5 s: sub_8265BEE8 {}, sub_82527990 {}, sub_8265D160 {}, sub_826605D8 {}, "
                  "sub_825D9538 {}", g_tick_calls[0].exchange(0), g_tick_calls[1].exchange(0),
                  g_tick_calls[2].exchange(0), g_tick_calls[3].exchange(0), g_tick_calls[4].exchange(0));
    }
  }
  if (which == g_tick_choice && xmd::debug::Waiting()) RunPending(ctx, base, g_main_state.load());
}

}  // namespace

#define XMD_TICK_HOOK(n, name)          REX_EXTERN(__imp__##name);            REX_HOOK_RAW(name) {                    Tick(n, ctx, base);                   __imp__##name(ctx, base);           }
XMD_TICK_HOOK(0, sub_8265BEE8)
XMD_TICK_HOOK(1, sub_82527990)
XMD_TICK_HOOK(2, sub_8265D160)
XMD_TICK_HOOK(3, sub_826605D8)
XMD_TICK_HOOK(4, sub_825D9538)
