// Test aid: where the render thread's time goes.
//
// XMD_PROFILE_TRIGGER=<file>: when the file appears, the thread that presents frames is sampled for
// XMD_PROFILE_SECONDS (5) - its call stack, about 4000 times a second - and the samples are written to
// XMD_PROFILE_FILE (profile.txt) as ms a frame: by module, by function (its own time, with who called it), and
// by function with everything it calls. The exe's PDB names the functions: recompiled game functions are
// sub_XXXXXXXX. tools\route_run.ps1 sets the three (runs\profile_now.txt -> runs\profile.txt).
#include "profile_sampler.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xmd::profile {
namespace {

constexpr int kDepth = 28;
using Stack = std::array<uint64_t, kDepth>;

std::atomic<uint64_t> g_frames{0};
HANDLE g_thread = nullptr;

double Now() {
  static LARGE_INTEGER frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) / double(frequency.QuadPart);
}

// The suspended thread's return addresses, innermost first (0 ends them). Its memory is read as it stands: a
// stack that can't be followed (a frame without unwind data in the middle of a prologue) just ends early.
int Unwind(const CONTEXT& start, uint64_t* frames) {
  int n = 0;
  __try {
    CONTEXT c = start;
    frames[n++] = c.Rip;
    while (n < kDepth) {
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

struct Names {
  HANDLE process = GetCurrentProcess();
  std::unordered_map<uint64_t, std::pair<std::string, std::string>> known;  // address -> (module, function)
  const std::pair<std::string, std::string>& Of(uint64_t address) {
    auto [it, added] = known.try_emplace(address);
    if (!added) return it->second;
    char module[MAX_PATH] = "?";
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &hm))
      GetModuleFileNameA(hm, module, MAX_PATH);
    const char* slash = std::strrchr(module, '\\');
    it->second.first = slash ? slash + 1 : module;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
      std::string name = symbol->Name;
      if (name.size() > 90) name = name.substr(0, 60) + "..." + name.substr(name.size() - 24);
      it->second.second = it->second.first == "xmd.exe" ? name : it->second.first + "!" + name;
    } else {
      char text[64];
      std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>((address - DWORD64(hm)) & ~0xFFFull));
      it->second.second = it->second.first + text;
    }
    return it->second;
  }
  std::string Line(uint64_t address) {
    IMAGEHLP_LINE64 info = {sizeof(info)};
    DWORD displacement = 0;
    if (!SymGetLineFromAddr64(process, address, &displacement, &info)) return {};
    const char* file = std::strrchr(info.FileName, '\\');
    char text[200];
    std::snprintf(text, sizeof(text), "%s:%lu", file ? file + 1 : info.FileName, info.LineNumber);
    return text;
  }
};

void Report(const std::vector<Stack>& samples, double seconds, uint64_t frames, const char* file) {
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  Names names;
  struct Function {
    uint64_t own = 0, all = 0;
    std::map<std::string, uint64_t> lines, callers;
  };
  std::unordered_map<std::string, Function> functions;
  std::unordered_map<std::string, uint64_t> modules;
  const uint64_t total = samples.size();
  for (const Stack& s : samples) {
    const auto& leaf = names.Of(s[0]);
    ++modules[leaf.first];
    Function& f = functions[leaf.second];
    ++f.own;
    const std::string line = names.Line(s[0]);
    if (!line.empty()) ++f.lines[line];
    std::string chain;
    std::unordered_set<std::string> seen;
    std::string last = leaf.second;
    int shown = 0;
    for (int i = 0; i < kDepth && s[i]; ++i) {
      const std::string& name = names.Of(s[i]).second;
      if (seen.insert(name).second) ++functions[name].all;
      if (i && shown < 4 && name != last) {
        chain += (shown ? " < " : "") + name;
        last = name;
        ++shown;
      }
    }
    if (!chain.empty()) ++f.callers[chain];
  }
  FILE* out = std::fopen(file, "w");
  if (!out) return;
  const double frame_ms = frames ? seconds * 1000.0 / double(frames) : 0.0;
  std::fprintf(out, "%.2f s, %llu samples, %llu frames: %.2f ms a frame, %.1f fps\n", seconds,
               static_cast<unsigned long long>(total), static_cast<unsigned long long>(frames), frame_ms,
               frames ? double(frames) / seconds : 0.0);
  auto ms = [&](uint64_t count) { return total ? frame_ms * double(count) / double(total) : 0.0; };
  std::fprintf(out, "\nby module (ms a frame):\n");
  std::vector<std::pair<std::string, uint64_t>> by_module(modules.begin(), modules.end());
  std::sort(by_module.begin(), by_module.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [name, count] : by_module) std::fprintf(out, "  %7.3f  %s\n", ms(count), name.c_str());

  std::vector<std::pair<std::string, const Function*>> order;
  for (const auto& [name, f] : functions) order.emplace_back(name, &f);
  std::fprintf(out, "\nby function with all it calls (ms a frame):\n");
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second->all > b.second->all; });
  for (size_t i = 0; i < order.size() && i < 90; ++i)
    std::fprintf(out, "  %7.3f  %s\n", ms(order[i].second->all), order[i].first.c_str());

  std::fprintf(out, "\nby function, its own time (ms a frame), with its busiest lines (:) and callers (<):\n");
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second->own > b.second->own; });
  for (size_t i = 0; i < order.size() && i < 220 && order[i].second->own; ++i) {
    const Function& f = *order[i].second;
    std::fprintf(out, "  %7.3f  %s\n", ms(f.own), order[i].first.c_str());
    if (f.own * 250 < total) continue;  // (under 0.4%: no detail)
    auto top = [&](const std::map<std::string, uint64_t>& map, size_t most, char mark) {
      std::vector<std::pair<std::string, uint64_t>> v(map.begin(), map.end());
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      for (size_t k = 0; k < v.size() && k < most; ++k)
        std::fprintf(out, "           %7.3f %c %s\n", ms(v[k].second), mark, v[k].first.c_str());
    };
    top(f.lines, f.own * 20 >= total ? 40 : 6, ':');
    top(f.callers, 4, '<');
  }
  std::fclose(out);
}

void Run(std::string trigger) {
  for (;;) {
    Sleep(250);
    if (GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    DeleteFileA(trigger.c_str());
    const char* seconds_text = std::getenv("XMD_PROFILE_SECONDS");
    const double seconds = seconds_text ? std::max(0.5, std::atof(seconds_text)) : 5.0;
    const char* file = std::getenv("XMD_PROFILE_FILE");
    std::vector<Stack> samples;
    samples.reserve(size_t(seconds * 4200));
    const uint64_t frames0 = g_frames.load();
    const double t0 = Now();
    double next = t0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (Now() - t0 < seconds) {
      while (Now() < next) YieldProcessor();
      next += 0.00025;
      if (SuspendThread(g_thread) == DWORD(-1)) continue;
      CONTEXT context;
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(g_thread, &context)) {
        Stack s = {};
        Unwind(context, s.data());
        samples.push_back(s);
      }
      ResumeThread(g_thread);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    const double elapsed = Now() - t0;
    Report(samples, elapsed, g_frames.load() - frames0, file ? file : "profile.txt");
  }
}

}  // namespace

void FrameThread() {
  static const char* trigger = std::getenv("XMD_PROFILE_TRIGGER");
  if (!trigger) return;
  ++g_frames;
  static const bool started = [] {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (g_thread) std::thread(Run, std::string(trigger)).detach();
    return true;
  }();
  (void)started;
}

}  // namespace xmd::profile

#else

namespace xmd::profile {
void FrameThread() {}
}  // namespace xmd::profile

#endif
