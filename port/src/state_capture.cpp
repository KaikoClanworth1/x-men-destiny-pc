// X-Men: Destiny - the whole state at an F4 capture (see state_capture.h).

#include "state_capture.h"

#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>
#include <rex/system/xmodule.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "debug_commands.h"

namespace xmd {

namespace {

constexpr int kHostDepth = 64;
constexpr int kGuestDepth = 64;
constexpr uint32_t kChunk = 0x10000;

struct Snap {
  rex::system::object_ref<rex::system::XThread> thread;
  HANDLE handle = nullptr;
  bool held = false, have_host = false, have_guest = false;
  CONTEXT host{};
  PPCContext guest;
  uint64_t host_frames[kHostDepth];
  int host_n = 0;
  uint32_t guest_frames[kGuestDepth];
  int guest_n = 0;
  uint32_t stack_base = 0, stack_limit = 0;
};

struct Region {
  uint32_t start, size, protect, heap;
};
struct Run {
  uint32_t start, size;
  uint64_t offset;
};

// The suspended thread's return addresses on the PC stack, innermost first (as tools\profile_sampler does).
int Unwind(const CONTEXT& start, uint64_t* frames) {
  int n = 0;
  __try {
    CONTEXT c = start;
    frames[n++] = c.Rip;
    while (n < kHostDepth) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
      if (function) {
        void* data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, function, &c, &data, &establisher, nullptr);
      } else {
        c.Rip = *reinterpret_cast<const uint64_t*>(c.Rsp);
        c.Rsp += 8;
      }
      if (c.Rip < 0x10000) break;
      frames[n++] = c.Rip;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return _byteswap_ulong(v);
}

// The PowerPC back chain: [r1] is the caller's frame, and the saved link register sits 8 below it (the game's
// prologues store LR at -8 of the frame they were called with).
int GuestWalk(const uint8_t* base, uint32_t sp, uint32_t limit, uint32_t top, uint32_t* frames) {
  int n = 0;
  __try {
    while (n < kGuestDepth && sp >= limit && sp + 4 <= top) {
      const uint32_t next = Load32(base, sp);
      if (next <= sp || next > top) break;
      frames[n++] = Load32(base, next - 8);
      sp = next;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

std::string Symbol(uint64_t address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 511;
  DWORD64 displacement = 0;
  char module[MAX_PATH] = "?";
  HMODULE hm = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &hm))
    GetModuleFileNameA(hm, module, MAX_PATH);
  const char* slash = std::strrchr(module, '\\');
  const std::string mod = slash ? slash + 1 : module;
  std::string text;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
    text = (mod == "xmd.exe" ? std::string() : mod + "!") + symbol->Name;
    char off[32];
    std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(displacement));
    text += off;
  } else {
    char off[64];
    std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(address - DWORD64(hm)));
    text = mod + off;
  }
  IMAGEHLP_LINE64 line = {sizeof(line)};
  DWORD line_displacement = 0;
  if (SymGetLineFromAddr64(GetCurrentProcess(), address, &line_displacement, &line)) {
    const char* file = std::strrchr(line.FileName, '\\');
    text += "  (" + std::string(file ? file + 1 : line.FileName) + ":" + std::to_string(line.LineNumber) + ")";
  }
  return text;
}

const char* TypeName(rex::system::XObject::Type type) {
  using T = rex::system::XObject::Type;
  switch (type) {
    case T::Enumerator: return "enumerator";
    case T::Event: return "event";
    case T::File: return "file";
    case T::IOCompletion: return "io-completion";
    case T::Module: return "module";
    case T::Mutant: return "mutant";
    case T::NotifyListener: return "notify-listener";
    case T::Semaphore: return "semaphore";
    case T::Session: return "session";
    case T::Socket: return "socket";
    case T::SymbolicLink: return "symbolic-link";
    case T::Thread: return "thread";
    case T::Timer: return "timer";
    default: return "undefined";
  }
}

// The script state's variables (not its 500-odd engine functions), as "name = type: value" lines.
const char kLuaGlobals[] =
    "local o,n,seen={},0,{} "
    "local function d(t,p,depth) "
    " if seen[t] or n>60000 then return end seen[t]=true "
    " for k,v in pairs(t) do "
    "  local ty=type(v) local name=p..tostring(k) "
    "  if ty=='table' then "
    "   if depth<5 then n=n+1 o[n]=name..' = table' d(v,name..'.',depth+1) else n=n+1 o[n]=name..' = table (deeper)' end "
    "  elseif ty~='function' and ty~='cfunction' then n=n+1 o[n]=name..' = '..ty..': '..tostring(v) end "
    " end "
    "end "
    "d(_G,'',0) "
    "return table.concat(o,'\\n')";

}  // namespace

std::string CaptureState(const std::filesystem::path& dir, const std::filesystem::path& user_data) {
  using namespace rex::system;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return "state: no kernel yet";
  rex::memory::Memory* memory = kernel->memory();
  const uint8_t* membase = memory->virtual_membase();

  // 1. The script state's variables - asked of the game's own script thread, before anything is held.
  {
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto text = std::make_shared<std::string>();
    auto lock = std::make_shared<std::mutex>();
    debug::Query(kLuaGlobals, [done, text, lock](bool ok, const std::string& t) {
      std::lock_guard guard(*lock);
      *text = ok ? t : "(the script state answered with an error: " + t + ")";
      *done = true;
    });
    for (int i = 0; i < 60 && !*done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ofstream out(dir / "lua_globals.txt", std::ios::binary);
    std::lock_guard guard(*lock);
    out << (*done ? *text : std::string("(the game's scripts did not answer within 3 s)")) << "\n";
  }

  // 2. The memory map and the threads, all allocated now: while the game is held nothing here may allocate
  // (a held thread may own the heap's lock).
  std::vector<Region> regions;
  regions.reserve(1 << 16);
  const uint32_t heap_bases[] = {0x00000000, 0x40000000, 0x80000000, 0x90000000,
                                 0xA0000000, 0xC0000000, 0xE0000000};
  for (uint32_t index = 0; index < std::size(heap_bases); ++index) {
    auto* heap = memory->LookupHeap(heap_bases[index] + 0x1000);
    if (!heap || heap->heap_base() != heap_bases[index]) continue;
    uint64_t at = heap->heap_base();
    const uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
    while (at < end && regions.size() < regions.capacity()) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(static_cast<uint32_t>(at), &info) || !info.region_size) break;
      if ((info.state & rex::memory::kMemoryAllocationCommit) && info.protect)
        regions.push_back({info.base_address, info.region_size, info.protect, index});
      at = uint64_t(info.base_address) + info.region_size;
    }
  }
  std::vector<Run> runs;
  runs.reserve(1 << 17);
  std::vector<uint8_t> buffer(kChunk);

  auto threads = kernel->object_table()->GetObjectsByType<XThread>();
  std::vector<std::unique_ptr<Snap>> snaps;
  const DWORD self = GetCurrentThreadId();
  for (auto& t : threads) {
    if (!t->is_guest_thread() || !t->thread()) continue;
    // (an object with more than one handle comes once per handle)
    bool seen = false;
    for (auto& s : snaps) seen = seen || s->thread.get() == t.get();
    if (seen) continue;
    auto s = std::make_unique<Snap>();
    s->handle = static_cast<HANDLE>(t->thread()->native_handle());
    if (GetThreadId(s->handle) == self) continue;
    if (auto* k = t->guest_object<X_KTHREAD>()) {
      s->stack_base = k->stack_base;
      s->stack_limit = k->stack_limit;
    }
    s->thread = t;
    snaps.push_back(std::move(s));
  }

  const HANDLE file = CreateFileW((dir / "memory.bin").wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

  // 3. Held: every game thread stopped where it is.
  const auto t0 = std::chrono::steady_clock::now();
  for (auto& s : snaps) s->held = SuspendThread(s->handle) != DWORD(-1);
  for (auto& s : snaps) {
    if (!s->held) continue;
    s->host.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(s->handle, &s->host)) {
      s->have_host = true;
      s->host_n = Unwind(s->host, s->host_frames);
    }
    if (auto* state = s->thread->thread_state(); state && state->context()) {
      s->guest = *state->context();
      s->have_guest = true;
      s->guest_n = GuestWalk(membase, s->guest.r1.u32, s->stack_limit, s->stack_base, s->guest_frames);
    }
  }
  uint64_t written = 0;
  if (file != INVALID_HANDLE_VALUE) {
    for (const Region& r : regions) {
      for (uint64_t at = r.start; at < uint64_t(r.start) + r.size; at += kChunk) {
        const uint32_t size = static_cast<uint32_t>(std::min<uint64_t>(kChunk, uint64_t(r.start) + r.size - at));
        SIZE_T got = 0;
        const void* host = memory->TranslateVirtual<const uint8_t*>(static_cast<uint32_t>(at));
        if (!ReadProcessMemory(GetCurrentProcess(), host, buffer.data(), size, &got) || got != size) continue;
        bool zero = true;
        for (uint32_t i = 0; i < size && zero; i += 8) {
          uint64_t v;
          std::memcpy(&v, buffer.data() + i, 8);
          zero = v == 0;
        }
        if (zero) continue;
        DWORD out = 0;
        if (!WriteFile(file, buffer.data(), size, &out, nullptr) || out != size) break;
        if (!runs.empty() && runs.back().start + runs.back().size == at &&
            runs.back().offset + runs.back().size == written) {
          runs.back().size += size;
        } else if (runs.size() < runs.capacity()) {
          runs.push_back({static_cast<uint32_t>(at), size, written});
        }
        written += size;
      }
    }
    CloseHandle(file);
  }
  for (auto& s : snaps)
    if (s->held) ResumeThread(s->handle);
  const double held_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  // 4. Written out, the game running again.
  {
    std::ofstream out(dir / "memory_map.txt", std::ios::binary);
    out << "# guest_start guest_end file_offset  (memory.bin holds these ranges back to back; what is not listed is\n"
           "# all zero or not committed)\n";
    char line[96];
    for (const Run& r : runs) {
      std::snprintf(line, sizeof(line), "%08X %08X %llX\n", r.start, r.start + r.size,
                    static_cast<unsigned long long>(r.offset));
      out << line;
    }
    out << "\n# committed regions: start size protect heap\n";
    for (const Region& r : regions) {
      std::snprintf(line, sizeof(line), "region %08X %08X %X %08X\n", r.start, r.size, r.protect,
                    heap_bases[r.heap]);
      out << line;
    }
  }

  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  {
    std::ofstream out(dir / "threads.txt", std::ios::binary);
    char line[512];
    for (auto& s : snaps) {
      auto& t = s->thread;
      // (the game's names - SetThreadName - are on the PC thread)
      std::string name = t->name();
      PWSTR description = nullptr;
      if (name.empty() && SUCCEEDED(GetThreadDescription(s->handle, &description)) && description) {
        for (const wchar_t* c = description; *c; ++c) name += *c < 128 ? char(*c) : '?';
        LocalFree(description);
      }
      std::snprintf(line, sizeof(line), "=== thread %08X id %u '%s' priority %d, stack %08X-%08X%s\n", t->handle(),
                    t->thread_id(), name.c_str(), t->priority(), s->stack_limit, s->stack_base,
                    s->held ? "" : " (could not be held)");
      out << line;
      if (s->have_guest) {
        const PPCContext& c = s->guest;
        std::snprintf(line, sizeof(line), "  lr %08X ctr %08X  r1 %08X r2 %08X r13 %08X\n",
                      static_cast<uint32_t>(c.lr), c.ctr.u32, c.r1.u32, c.r2.u32, c.r13.u32);
        out << line;
        const uint64_t r[32] = {c.r0.u64,  c.r1.u64,  c.r2.u64,  c.r3.u64,  c.r4.u64,  c.r5.u64,  c.r6.u64,
                                c.r7.u64,  c.r8.u64,  c.r9.u64,  c.r10.u64, c.r11.u64, c.r12.u64, c.r13.u64,
                                c.r14.u64, c.r15.u64, c.r16.u64, c.r17.u64, c.r18.u64, c.r19.u64, c.r20.u64,
                                c.r21.u64, c.r22.u64, c.r23.u64, c.r24.u64, c.r25.u64, c.r26.u64, c.r27.u64,
                                c.r28.u64, c.r29.u64, c.r30.u64, c.r31.u64};
        for (int i = 0; i < 32; i += 4) {
          std::snprintf(line, sizeof(line), "  r%-2d %016llX  r%-2d %016llX  r%-2d %016llX  r%-2d %016llX\n", i,
                        static_cast<unsigned long long>(r[i]), i + 1, static_cast<unsigned long long>(r[i + 1]),
                        i + 2, static_cast<unsigned long long>(r[i + 2]), i + 3,
                        static_cast<unsigned long long>(r[i + 3]));
          out << line;
        }
        const double f[8] = {c.f0.f64, c.f1.f64, c.f2.f64, c.f3.f64, c.f4.f64, c.f5.f64, c.f6.f64, c.f7.f64};
        std::snprintf(line, sizeof(line), "  f0-f7 %g %g %g %g %g %g %g %g\n", f[0], f[1], f[2], f[3], f[4], f[5],
                      f[6], f[7]);
        out << line;
        out << "  PowerPC return addresses on its stack (innermost first):";
        for (int i = 0; i < s->guest_n; ++i) {
          std::snprintf(line, sizeof(line), " %08X", s->guest_frames[i]);
          out << line;
        }
        out << "\n";
      }
      if (s->have_host) {
        out << "  where it is (PC call stack; sub_XXXXXXXX are the game's own functions):\n";
        for (int i = 0; i < s->host_n; ++i) out << "    " << Symbol(s->host_frames[i]) << "\n";
      }
      out << "\n";
    }
  }

  {
    std::ofstream out(dir / "objects.txt", std::ios::binary);
    char line[160];
    for (auto& o : kernel->object_table()->GetAllObjects()) {
      std::snprintf(line, sizeof(line), "%08X %-16s guest %08X  ", o->handle(), TypeName(o->type()),
                    o->guest_object());
      out << line << o->name();
      if (o->type() == XObject::Type::File) {
        auto* f = static_cast<XFile*>(o.get());
        out << "  path " << f->path() << " at " << f->position();
      } else if (o->type() == XObject::Type::Module) {
        out << "  " << static_cast<XModule*>(o.get())->path();
      }
      out << "\n";
    }
  }

  // The saved games as they stand (the profile folders: 16 hex digits).
  for (const auto& entry : std::filesystem::directory_iterator(user_data, ec)) {
    const std::string name = entry.path().filename().string();
    if (!entry.is_directory() || name.size() != 16) continue;
    std::filesystem::create_directories(dir / "save", ec);
    std::filesystem::copy(entry.path(), dir / "save" / name,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          ec);
  }

  char summary[256];
  std::snprintf(summary, sizeof(summary),
                "state: %.0f MB of memory in %zu ranges (%zu committed regions), %zu threads; the game was held "
                "%.0f ms",
                double(written) / (1024.0 * 1024.0), runs.size(), regions.size(), snaps.size(), held_ms);
  return summary;
}

}  // namespace xmd
