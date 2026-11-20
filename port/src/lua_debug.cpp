// X-Men: Destiny - what the game's menus and scripts ask for, in the log.
//
// The front end is Flash (Scaleform) calling into the game with
// ExternalInterface.call(name); calls no menu handler takes go to a Lua global
// of that name. A missing one is a script error the engine catches (lua_pcall),
// so these lines are information, not failures.
//
//   sub_826A20B8  luaG_runerror(L, fmt, ...)   r3 = L, r4 = fmt, r5.. = arguments
//   sub_8271A058  the Flash call dispatcher     r5 = name, r7 = argument count

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/xmd_init.h"
#include "pc_menu.h"
#include "problem_log.h"

namespace {

// A guest C string.
std::string GuestString(uint8_t* base, uint32_t address, size_t limit = 300) {
  std::string out;
  if (address < 0x10000) return "(null)";
  const char* p = reinterpret_cast<const char*>(base + address);
  for (size_t i = 0; i < limit && p[i]; ++i) out.push_back(p[i]);
  return out;
}

// The Flash call this thread is inside of (the dispatcher's hook below), or empty.
thread_local std::string g_flash_call;

}  // namespace

REX_EXTERN(__imp__sub_826A20B8);
REX_HOOK_RAW(sub_826A20B8) {
  const std::string fmt = GuestString(base, ctx.r4.u32);
  const uint32_t args[] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  std::string text;
  size_t next = 0;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%' || i + 1 >= fmt.size()) {
      text.push_back(fmt[i]);
      continue;
    }
    const char kind = fmt[++i];
    const uint32_t value = next < 6 ? args[next] : 0;
    if (kind == 's') {
      // (only addresses the game image or its heaps can hold)
      text += value >= 0x40000000 && value < 0xA0000000 ? GuestString(base, value) : "?";
      ++next;
    } else if (kind == 'd') {
      text += std::to_string(static_cast<int32_t>(value));
      ++next;
    } else if (kind == '%') {
      text.push_back('%');
    } else {
      text.push_back('%');
      text.push_back(kind);
      ++next;
    }
  }
  REXLOG_DEBUG("lua error: {}", text);
  if (!g_flash_call.empty() && text == "attempt to call a nil value") {
    // A menu call no menu handler took and no script has a function for (MainMenuIntroActivated, NotifyReady,
    // AcceptAction, ...): the game looks the name up as a Lua global, calls it, and catches the error - on the
    // console too. Not a problem: once a name in the log, none in the problems log.
    static std::mutex mutex;
    static std::set<std::string> told;
    bool first;
    {
      std::lock_guard lock(mutex);
      first = told.insert(g_flash_call).second;
    }
    if (first) REXLOG_INFO("flash call: {} has no handler and no script function (the game's own; ignored)", g_flash_call);
  } else {
    xmd::problems::Report("script", "Lua error: " + text);
  }
  __imp__sub_826A20B8(ctx, base);
}

// Each Flash call's name once.
REX_EXTERN(__imp__sub_8271A058);
REX_HOOK_RAW(sub_8271A058) {
  static std::mutex mutex;
  static std::set<std::string> seen;
  const std::string name = GuestString(base, ctx.r5.u32, 120);
  bool first;
  {
    std::lock_guard lock(mutex);
    first = seen.insert(name).second;
  }
  if (first) REXLOG_INFO("flash call: {} ({} argument(s))", name, ctx.r7.u32);
  if (xmd::pcmenu::OnFlashCall(ctx, base)) return;  // (the port's own menu entries: pc_menu.h)
  const std::string outer = g_flash_call;
  g_flash_call = name;
  __imp__sub_8271A058(ctx, base);
  g_flash_call = outer;
}
