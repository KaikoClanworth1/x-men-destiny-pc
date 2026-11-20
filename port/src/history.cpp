// X-Men: Destiny - the history (see history.h).

#include "history.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <rex/hook.h>
#include <rex/logging.h>

namespace xmd::history {

namespace {

constexpr double kKeepSeconds = 300.0;

bool g_on = false;
const uint8_t* g_base = nullptr;
uint64_t g_tsc0 = 0;
std::chrono::steady_clock::time_point g_time0;

uint64_t Tsc() { return __builtin_ia32_rdtsc(); }

// Ticks of the time stamp counter a second: measured against the clock since the start.
double TscPerSecond() {
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_time0).count();
  return s > 0.05 ? double(Tsc() - g_tsc0) / s : 3.0e9;
}

void* Reserve(size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

// Guest memory that may not be there (a pointer argument of a call that failed).
bool GuestRead(uint32_t address, void* out, size_t size) {
  if (!g_base || address < 0x10000) return false;
  __try {
    std::memcpy(out, g_base + address, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
uint32_t GuestU32(uint32_t address) {
  uint32_t v = 0;
  return GuestRead(address, &v, 4) ? _byteswap_ulong(v) : 0;
}

// --- Events ------------------------------------------------------------------------------------------------

struct EventLine {
  uint64_t tsc;
  uint32_t thread;
  std::string text;
};
std::mutex g_event_mutex;
std::deque<EventLine> g_events;
constexpr size_t kMaxEvents = 400000;

void AddEvent(uint64_t tsc, std::string text) {
  std::lock_guard lock(g_event_mutex);
  g_events.push_back({tsc, GetCurrentThreadId(), std::move(text)});
  if (g_events.size() > kMaxEvents) g_events.pop_front();
}

// --- Graphics calls ----------------------------------------------------------------------------------------

struct Call {
  uint64_t tsc;
  uint32_t frame;
  uint8_t type, flag;
  uint16_t pad;
  uint32_t a, b, c, d;
};
Call* g_calls = nullptr;
uint64_t g_call_mask = 0;
std::atomic<uint64_t> g_call_next{0};
const char* const* g_call_names = nullptr;
uint32_t g_call_name_count = 0;
constexpr uint32_t kMaxKinds = 24;
uint32_t g_kind_count[kMaxKinds];  // (the frame under way)

// --- Frames ------------------------------------------------------------------------------------------------

struct FrameRecord {
  uint64_t tsc;  // (its end)
  float ms, renderer_ms, waited_ms, slow_ms, built_ms;
  const char* slow;
  uint32_t draws, device_copies, calls;
  uint16_t kinds[kMaxKinds];
};
constexpr uint64_t kFrameRing = 1u << 17;
FrameRecord* g_frames = nullptr;
std::atomic<uint64_t> g_frame_next{0};
uint64_t g_last_frame_tsc = 0;
double g_built_ms = 0;
thread_local bool t_frame_thread = false;

// --- Kernel calls ------------------------------------------------------------------------------------------

enum Class : uint8_t { kUnknown, kBoring, kPlain, kWait, kMemory, kFile, kThread };

struct KernelCall {
  uint64_t tsc;        // (its start)
  uint32_t ticks;      // (its length, time stamp ticks / 64)
  uint32_t result;
  const char* name;
  uint32_t args[4];
  uint32_t lr;
  uint32_t pad;
};
constexpr uint32_t kThreadRing = 1u << 14;

struct NameStat {
  const char* name = nullptr;
  Class cls = kUnknown;
  uint64_t calls = 0, ticks = 0, longest = 0;
};
constexpr uint32_t kNameSlots = 1024;

struct ThreadHistory {
  uint32_t thread = 0;
  bool frame_thread = false;
  KernelCall* ring = nullptr;
  uint64_t next = 0;
  NameStat names[kNameSlots];
};
std::mutex g_thread_mutex;
std::vector<ThreadHistory*> g_threads;  // (never freed: a thread that ended keeps what it did)

Class Classify(const char* name) {
  const char* n = std::strncmp(name, "__imp__", 7) == 0 ? name + 7 : name;
  auto has = [n](const char* part) { return std::strstr(n, part) != nullptr; };
  if (has("CriticalSection") || has("Tls") || has("SpinLock") || has("GetCurrentProcessType") ||
      has("KeGetCurrentProcessType") || has("QueryPerformance") || has("KeQuerySystemTime") ||
      has("InterlockedPush") || has("InterlockedPop") || has("InterlockedFlush") || has("XGetLanguage") ||
      has("KeEnterCriticalRegion") || has("KeLeaveCriticalRegion") || has("RtlInitAnsiString") ||
      has("KeRaiseIrql") || has("KfLowerIrql") || has("KfRaiseIrql") || has("KeLowerIrql"))
    return kBoring;
  if (has("Wait") || has("Delay") || has("Sleep") || has("NtYieldExecution") || has("XamInputGetState") ||
      has("VdSwap") || has("NtRemoveIoCompletion"))
    return kWait;
  if (has("VirtualMemory") || has("PhysicalMemory") || has("MmQuery") || has("MmSetAddressProtect") ||
      has("XMem") || has("AllocatePool") || has("FreePool") || has("MmMapIoSpace") || has("Heap"))
    return kMemory;
  if (has("File") || has("NtClose") || has("NtQueryDirectory") || has("NtQueryVolume") || has("NtQueryInformation") ||
      has("XamContent") || has("NtSetInformation") || has("IoCreate") || has("NtDeviceIoControl"))
    return kFile;
  if (has("Thread") || has("ExRegisterTitleTerminate")) return kThread;
  return kPlain;
}

ThreadHistory* MyThread() {
  thread_local ThreadHistory* mine = nullptr;
  if (!mine) {
    auto* t = new ThreadHistory();
    t->thread = GetCurrentThreadId();
    t->ring = static_cast<KernelCall*>(Reserve(sizeof(KernelCall) * kThreadRing));
    std::lock_guard lock(g_thread_mutex);
    g_threads.push_back(t);
    mine = t;
  }
  return mine;
}

// Handle -> path of the files opened (for the reads' lines).
std::mutex g_file_mutex;
std::unordered_map<uint32_t, std::string> g_files;

std::string GuestPath(uint32_t object_attributes) {
  // (OBJECT_ATTRIBUTES: root, ANSI_STRING* name, attributes; ANSI_STRING: u16 length, u16 max, char* buffer)
  const uint32_t name = GuestU32(object_attributes + 4);
  uint16_t length_be = 0;
  if (!name || !GuestRead(name, &length_be, 2)) return "?";
  const uint32_t length = std::min<uint32_t>(_byteswap_ushort(length_be), 260);
  const uint32_t buffer = GuestU32(name + 4);
  std::string text(length, '\0');
  if (!length || !GuestRead(buffer, text.data(), length)) return "?";
  return text;
}

const char* Short(const char* name) { return std::strncmp(name, "__imp__", 7) == 0 ? name + 7 : name; }

// A memory, file or thread call, or one that took long: a line of words.
void DescribeCall(const char* name, Class cls, const PPCContext& ctx, const uint32_t* in, uint64_t begin,
                  double ms) {
  const char* n = Short(name);
  const uint32_t result = ctx.r3.u32;
  char text[560];
  if (!std::strcmp(n, "NtAllocateVirtualMemory")) {
    std::snprintf(text, sizeof(text), "memory: NtAllocateVirtualMemory -> %08X, %u KB at %08X (type %X, protect %X) %.2f ms, from %08X",
                  result, GuestU32(in[1]) / 1024, GuestU32(in[0]), in[2], in[3], ms, uint32_t(ctx.lr));
  } else if (!std::strcmp(n, "NtFreeVirtualMemory")) {
    std::snprintf(text, sizeof(text), "memory: NtFreeVirtualMemory -> %08X, %08X (%u KB, type %X) %.2f ms, from %08X", result,
                  GuestU32(in[0]), GuestU32(in[1]) / 1024, in[2], ms, uint32_t(ctx.lr));
  } else if (!std::strncmp(n, "MmAllocatePhysicalMemory", 24)) {
    std::snprintf(text, sizeof(text), "memory: %s %u KB (flags %X, protect %X) -> %08X %.2f ms, from %08X", n, in[1] / 1024,
                  in[0], in[2], result, ms, uint32_t(ctx.lr));
  } else if (!std::strcmp(n, "MmFreePhysicalMemory")) {
    std::snprintf(text, sizeof(text), "memory: MmFreePhysicalMemory %08X %.2f ms, from %08X", in[1], ms, uint32_t(ctx.lr));
  } else if (!std::strcmp(n, "NtCreateFile") || !std::strcmp(n, "NtOpenFile")) {
    const std::string path = GuestPath(in[2]);
    const uint32_t handle = GuestU32(in[0]);
    if (result == 0 && handle) {
      std::lock_guard lock(g_file_mutex);
      if (g_files.size() > 4096) g_files.clear();
      g_files[handle] = path;
    }
    std::snprintf(text, sizeof(text), "file: %s \"%s\" -> %08X, handle %08X %.2f ms", n, path.c_str(), result, handle, ms);
  } else if (!std::strcmp(n, "NtReadFile") || !std::strcmp(n, "NtWriteFile")) {
    std::string path = "?";
    {
      std::lock_guard lock(g_file_mutex);
      if (const auto it = g_files.find(in[0]); it != g_files.end()) path = it->second;
    }
    uint32_t offset[2] = {0, 0};
    const bool has_offset = ctx.r10.u32 && GuestRead(ctx.r10.u32, offset, 8);
    std::snprintf(text, sizeof(text), "file: %s \"%s\" %u bytes at %s%08X%08X -> %08X %.2f ms", n, path.c_str(), ctx.r9.u32,
                  has_offset ? "" : "(its position) ", _byteswap_ulong(offset[0]), _byteswap_ulong(offset[1]), result, ms);
  } else if (!std::strcmp(n, "NtClose")) {
    std::string path;
    {
      std::lock_guard lock(g_file_mutex);
      if (const auto it = g_files.find(in[0]); it != g_files.end()) path = it->second, g_files.erase(it);
    }
    if (path.empty() && ms < 2.0) return;  // (not a file's handle: an event's, a thread's)
    std::snprintf(text, sizeof(text), "file: NtClose \"%s\" handle %08X %.2f ms", path.c_str(), in[0], ms);
  } else {
    const char* word = cls == kMemory ? "memory" : cls == kFile ? "file" : cls == kThread ? "thread" :
                       cls == kWait ? "wait (the frame's thread)" : "slow call";
    std::snprintf(text, sizeof(text), "%s: %s(%08X, %08X, %08X, %08X) -> %08X %.2f ms, from %08X", word, n, in[0], in[1],
                  in[2], in[3], result, ms, uint32_t(ctx.lr));
  }
  AddEvent(begin, text);
}

double g_ticks_per_ms = 3.0e6;  // (refreshed at each frame's end)

void OnKernelCall(const char* name, const PPCContext& ctx, const uint32_t* in, uint64_t begin) {
  const uint64_t ticks = Tsc() - begin;
  ThreadHistory* t = MyThread();
  // (the name's slot: by its address - the string of one REX_EXPORT)
  uint32_t slot = uint32_t((reinterpret_cast<uintptr_t>(name) * 0x9E3779B97F4A7C15ull) >> 54) & (kNameSlots - 1);
  NameStat* s = &t->names[slot];
  for (int i = 0; s->name != name && s->name && i < 64; ++i) s = &t->names[slot = (slot + 1) & (kNameSlots - 1)];
  if (s->name != name) {
    if (s->name) return;  // (the table full: never, there are ~500 exports)
    s->name = name;
    s->cls = Classify(name);
  }
  ++s->calls;
  s->ticks += ticks;
  if (ticks > s->longest) s->longest = ticks;
  if (s->cls == kBoring) return;
  KernelCall& c = t->ring[t->next & (kThreadRing - 1)];
  c.tsc = begin;
  c.ticks = uint32_t(std::min<uint64_t>(ticks >> 6, 0xFFFFFFFFu));
  c.result = ctx.r3.u32;
  c.name = name;
  std::memcpy(c.args, in, sizeof(c.args));
  c.lr = uint32_t(ctx.lr);
  ++t->next;
  const double ms = double(ticks) / g_ticks_per_ms;
  const bool told = s->cls == kMemory || s->cls == kFile || s->cls == kThread ||
                    (s->cls == kWait ? t_frame_thread && ms >= 8.0 : ms >= 2.0);
  if (told) DescribeCall(name, s->cls, ctx, in, begin, ms);
}

std::string ThreadName(uint32_t id) {
  std::string name;
  if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id)) {
    PWSTR wide = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &wide)) && wide) {
      for (const wchar_t* p = wide; *p; ++p) name.push_back(*p < 128 ? char(*p) : '?');
      LocalFree(wide);
    }
    CloseHandle(h);
  } else {
    name = "(ended)";
  }
  return name;
}

}  // namespace

void Install(const uint8_t* guest_base) {
  if (std::getenv("XMD_NO_HISTORY")) return;
  g_base = guest_base;
  g_tsc0 = Tsc();
  g_time0 = std::chrono::steady_clock::now();
  uint64_t mb = 64;
  if (const char* v = std::getenv("XMD_HISTORY_CALLS_MB")) mb = std::clamp<uint64_t>(std::strtoull(v, nullptr, 10), 1, 8192);
  uint64_t entries = 1;
  while (entries * 2 * sizeof(Call) <= mb * 1048576) entries *= 2;
  g_calls = static_cast<Call*>(Reserve(entries * sizeof(Call)));
  g_frames = static_cast<FrameRecord*>(Reserve(kFrameRing * sizeof(FrameRecord)));
  if (!g_calls || !g_frames) return;
  g_call_mask = entries - 1;
  g_on = true;
  rex::SetExportTrace(&OnKernelCall);
  REXLOG_INFO("history: recording (the last {} graphics calls, 5 minutes of frames and events, the kernel calls of "
              "every thread) - F4 writes it out", entries);
}

void SetRenderCallNames(const char* const* names, uint32_t count) {
  g_call_names = names;
  g_call_name_count = count;
}

void RenderCall(uint8_t type, bool flag, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  if (!g_on) return;
  const uint64_t at = g_call_next.load(std::memory_order_relaxed);
  Call& e = g_calls[at & g_call_mask];
  e.tsc = Tsc();
  e.frame = uint32_t(g_frame_next.load(std::memory_order_relaxed));
  e.type = type, e.flag = flag;
  e.a = a, e.b = b, e.c = c, e.d = d;
  g_call_next.store(at + 1, std::memory_order_release);
  if (type < kMaxKinds) ++g_kind_count[type];
}

void FrameBuilt(double ms) { g_built_ms += ms; }

void FrameEnd(const Frame& frame) {
  if (!g_on) return;
  t_frame_thread = true;
  const uint64_t now = Tsc();
  const uint64_t at = g_frame_next.load(std::memory_order_relaxed);
  if ((at & 255) == 0) g_ticks_per_ms = TscPerSecond() / 1000.0;
  FrameRecord& r = g_frames[at & (kFrameRing - 1)];
  r.tsc = now;
  r.ms = g_last_frame_tsc ? float(double(now - g_last_frame_tsc) / g_ticks_per_ms) : 0;
  r.renderer_ms = frame.renderer_ms, r.waited_ms = frame.waited_ms, r.slow_ms = frame.slow_ms;
  r.built_ms = float(g_built_ms);
  r.slow = frame.slow;
  r.draws = frame.draws, r.device_copies = frame.device_copies;
  uint32_t calls = 0;
  for (uint32_t k = 0; k < kMaxKinds; ++k) {
    calls += g_kind_count[k];
    r.kinds[k] = uint16_t(std::min<uint32_t>(g_kind_count[k], 0xFFFF));
    g_kind_count[k] = 0;
  }
  r.calls = calls;
  g_built_ms = 0;
  g_last_frame_tsc = now;
  g_frame_next.store(at + 1, std::memory_order_release);
}

void Event(std::string_view category, std::string_view text) {
  if (!g_on) return;
  std::string line(category);
  line.append(": ").append(text.substr(0, 700));
  AddEvent(Tsc(), std::move(line));
}

std::string Write(const std::filesystem::path& dir) {
  if (!g_on) return "history: off";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const uint64_t now = Tsc();
  const double per_s = TscPerSecond();
  const uint64_t keep = uint64_t(kKeepSeconds * per_s);
  const uint64_t oldest = now > keep ? now - keep : 0;
  auto ago = [&](uint64_t tsc) { return -(double(now) - double(tsc)) / per_s; };
  auto open = [&](const char* name) {
    FILE* f = nullptr;
    _wfopen_s(&f, (dir / name).c_str(), L"wb");
    if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    return f;
  };
  auto kind = [](uint32_t type) { return type < g_call_name_count ? g_call_names[type] : "?"; };

  // Frames.
  uint64_t frames_written = 0;
  if (FILE* f = open("frames.csv")) {
    std::fprintf(f, "seconds,frame,ms,renderer_thread_ms,game_waited_ms,built_ms,slowest_call,slowest_ms,draws,calls,device_copies");
    for (uint32_t k = 0; k < g_call_name_count && k < kMaxKinds; ++k) std::fprintf(f, ",%s", kind(k));
    std::fputc('\n', f);
    const uint64_t end = g_frame_next.load(std::memory_order_acquire);
    for (uint64_t i = end > kFrameRing ? end - kFrameRing + 1 : 0; i < end; ++i) {
      const FrameRecord& r = g_frames[i & (kFrameRing - 1)];
      if (r.tsc < oldest) continue;
      std::fprintf(f, "%.4f,%llu,%.2f,%.2f,%.2f,%.2f,%s,%.2f,%u,%u,%u", ago(r.tsc), static_cast<unsigned long long>(i),
                   r.ms, r.renderer_ms, r.waited_ms, r.built_ms, r.slow ? r.slow : "", r.slow_ms, r.draws, r.calls,
                   r.device_copies);
      for (uint32_t k = 0; k < g_call_name_count && k < kMaxKinds; ++k) std::fprintf(f, ",%u", r.kinds[k]);
      std::fputc('\n', f);
      ++frames_written;
    }
    std::fclose(f);
  }

  // Events.
  size_t events_written = 0;
  if (FILE* f = open("events.txt")) {
    std::vector<EventLine> lines;
    {
      std::lock_guard lock(g_event_mutex);
      while (!g_events.empty() && g_events.front().tsc < oldest) g_events.pop_front();
      lines.assign(g_events.begin(), g_events.end());
    }
    std::stable_sort(lines.begin(), lines.end(), [](const EventLine& a, const EventLine& b) { return a.tsc < b.tsc; });
    std::fprintf(f, "# seconds before the capture, thread, what (memory, files, threads, calls that took long, "
                    "pipelines, stutters)\n");
    for (const EventLine& e : lines) std::fprintf(f, "%10.4f  t%-6u %s\n", ago(e.tsc), e.thread, e.text.c_str());
    events_written = lines.size();
    std::fclose(f);
  }

  // Graphics calls.
  uint64_t calls_written = 0;
  double calls_span = 0;
  if (FILE* f = open("render_calls.txt")) {
    std::fprintf(f, "# seconds before the capture, frame, call, flag, a b c d (a draw: primitive, start, count; a "
                    "command buffer: buffer, predication; ...)\n");
    const uint64_t end = g_call_next.load(std::memory_order_acquire);
    // (the oldest few are being written over while this reads: left out)
    const uint64_t begin = end > g_call_mask ? end - g_call_mask + 4096 : 0;
    uint32_t frame = ~0u;
    for (uint64_t i = begin; i < end; ++i) {
      const Call e = g_calls[i & g_call_mask];
      if (e.tsc < oldest || e.tsc > now) continue;
      if (!calls_written) calls_span = -ago(e.tsc);
      if (e.frame != frame) std::fprintf(f, "--- frame %u\n", frame = e.frame);
      std::fprintf(f, "%.5f %u %s %u %08X %08X %08X %08X\n", ago(e.tsc), e.frame, kind(e.type), e.flag, e.a, e.b,
                   e.c, e.d);
      ++calls_written;
    }
    std::fclose(f);
  }

  // Kernel calls: every thread's ring in one list by time, and the totals.
  struct Row {
    KernelCall call;
    uint32_t thread;
  };
  std::vector<Row> rows;
  std::unordered_map<const char*, NameStat> totals;
  std::vector<std::pair<uint32_t, uint64_t>> thread_calls;
  {
    std::vector<ThreadHistory*> threads;
    {
      std::lock_guard lock(g_thread_mutex);
      threads = g_threads;
    }
    for (const ThreadHistory* t : threads) {
      const uint64_t end = t->next;
      for (uint64_t i = end > kThreadRing ? end - kThreadRing + 16 : 0; i < end; ++i) {
        const KernelCall c = t->ring[i & (kThreadRing - 1)];
        if (c.name && c.tsc >= oldest && c.tsc <= now) rows.push_back({c, t->thread});
      }
      uint64_t all = 0;
      for (const NameStat& s : t->names) {
        if (!s.name) continue;
        NameStat& sum = totals[s.name];
        sum.name = s.name, sum.cls = s.cls;
        sum.calls += s.calls, sum.ticks += s.ticks;
        sum.longest = std::max(sum.longest, s.longest);
        all += s.calls;
      }
      thread_calls.emplace_back(t->thread, all);
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.call.tsc < b.call.tsc; });
  if (FILE* f = open("kernel_calls.txt")) {
    std::fprintf(f, "# seconds before the capture (the call's start), thread, ms, call(r3, r4, r5, r6) -> result, "
                    "called from\n# threads:");
    for (const auto& [id, calls] : thread_calls)
      std::fprintf(f, " t%u=\"%s\" (%llu calls)", id, ThreadName(id).c_str(), static_cast<unsigned long long>(calls));
    std::fputc('\n', f);
    for (const Row& r : rows) {
      const KernelCall& c = r.call;
      std::fprintf(f, "%10.5f t%-6u %8.3f %s(%08X, %08X, %08X, %08X) -> %08X from %08X\n", ago(c.tsc), r.thread,
                   double(uint64_t(c.ticks) << 6) / per_s * 1000.0, Short(c.name), c.args[0], c.args[1], c.args[2],
                   c.args[3], c.result, c.lr);
    }
    std::fclose(f);
  }
  if (FILE* f = open("kernel_summary.txt")) {
    std::vector<NameStat> list;
    for (const auto& [name, s] : totals) list.push_back(s);
    std::sort(list.begin(), list.end(), [](const NameStat& a, const NameStat& b) { return a.ticks > b.ticks; });
    std::fprintf(f, "# since the start: total ms, calls, average us, longest ms, call (waits count their waiting)\n");
    for (const NameStat& s : list) {
      std::fprintf(f, "%12.1f %12llu %10.2f %10.2f  %s%s\n", double(s.ticks) / per_s * 1000.0,
                   static_cast<unsigned long long>(s.calls), double(s.ticks) / per_s * 1.0e6 / double(s.calls),
                   double(s.longest) / per_s * 1000.0, Short(s.name), s.cls == kBoring ? "  (counted only)" : "");
    }
    std::fclose(f);
  }

  char text[400];
  std::snprintf(text, sizeof(text),
                "history: %llu frames and %zu events of the last 5 minutes, the last %llu graphics calls (%.1f s), "
                "%zu kernel calls of %zu threads",
                static_cast<unsigned long long>(frames_written), events_written,
                static_cast<unsigned long long>(calls_written), calls_span, rows.size(), thread_calls.size());
  return text;
}

}  // namespace xmd::history
