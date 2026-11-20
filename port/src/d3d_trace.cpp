// X-Men: Destiny - a census of the game's Direct3D library (developer build
// option XMD_D3D_TRACE, exe xmd_trace; hooks in d3d_trace_hooks.cpp from
// tools/gen_d3d_trace.py).
//
// XMD_D3D_TRACE_DIR=<folder> (required):
//   calls.csv        rewritten every 300 frames: per function, calls a frame over
//                    those frames and in total, and the range of r3..r6 it got
//   frame_<n>.txt    one whole frame, every library call in order with its
//                    arguments, nested calls indented, the returned r3 after "->".
//                    Written for the frame after the file <folder>\snap appears
//                    (the file is then deleted), so a test asks for it when the
//                    screen shows what it wants.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/xmd_init.h"

namespace xmd::d3d_trace {

extern const uint32_t kFunctionCount;
extern const uint32_t kFunctions[];

namespace {

constexpr uint32_t kMaxFunctions = 2048;
constexpr uint32_t kPerFunctionLogLimit = 1500;  // (a spin-wait runs 100,000s of times a frame)

std::atomic<uint64_t> g_calls[kMaxFunctions];
std::atomic<uint64_t> g_total[kMaxFunctions];
std::atomic<uint32_t> g_min[kMaxFunctions][4];
std::atomic<uint32_t> g_max[kMaxFunctions][4];
std::atomic<uint32_t> g_caller[kMaxFunctions];

struct Record {
  uint32_t index, depth, r[6], lr, result;
  bool returned;
};
std::atomic<bool> g_logging{false};
std::mutex g_mutex;
std::vector<Record> g_records;
uint32_t g_logged[kMaxFunctions];
thread_local uint32_t t_depth = 0;
thread_local std::vector<int32_t> t_open;  // record index per open call (-1: not logged)

std::string Dir() {
  const char* v = std::getenv("XMD_D3D_TRACE_DIR");
  return v ? v : "";
}

void WriteCounts(uint64_t frame) {
  const std::string path = Dir() + "\\calls.csv";
  FILE* out = std::fopen((path + ".tmp").c_str(), "w");
  if (!out) return;
  std::fprintf(out, "# frame %llu\naddress,per_frame,total,caller,r3_min,r3_max,r4_min,r4_max,r5_min,r5_max,r6_min,r6_max\n",
               static_cast<unsigned long long>(frame));
  for (uint32_t i = 0; i < kFunctionCount && i < kMaxFunctions; ++i) {
    const uint64_t n = g_calls[i].exchange(0);
    if (!g_total[i].load()) continue;
    std::fprintf(out, "%08X,%.2f,%llu,%08X", kFunctions[i], n / 300.0,
                 static_cast<unsigned long long>(g_total[i].load()), g_caller[i].load());
    for (int k = 0; k < 4; ++k) std::fprintf(out, ",%08X,%08X", g_min[i][k].load(), g_max[i][k].load());
    std::fputc('\n', out);
  }
  std::fclose(out);
  std::error_code ec;
  std::filesystem::rename(path + ".tmp", path, ec);
}

void WriteFrame(uint64_t frame) {
  std::vector<Record> records;
  {
    std::lock_guard lock(g_mutex);
    records.swap(g_records);
  }
  char name[64];
  std::snprintf(name, sizeof(name), "\\frame_%llu.txt", static_cast<unsigned long long>(frame));
  FILE* out = std::fopen((Dir() + name).c_str(), "w");
  if (!out) return;
  for (const Record& r : records) {
    std::fprintf(out, "%*s%08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X lr %08X", int(r.depth * 2), "",
                 kFunctions[r.index], r.r[0], r.r[1], r.r[2], r.r[3], r.r[4], r.r[5], r.lr);
    if (r.returned) std::fprintf(out, " -> %08X", r.result);
    std::fputc('\n', out);
  }
  std::fclose(out);
  REXLOG_INFO("d3d trace: frame {} written, {} calls", frame, records.size());
}

}  // namespace

void Enter(uint32_t index, const PPCContext& ctx) {
  if (index >= kMaxFunctions) return;
  g_calls[index].fetch_add(1, std::memory_order_relaxed);
  const uint64_t total = g_total[index].fetch_add(1, std::memory_order_relaxed);
  const uint32_t args[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  if (total < 100000) {
    for (int k = 0; k < 4; ++k) {
      if (!total || args[k] < g_min[index][k].load(std::memory_order_relaxed)) g_min[index][k] = args[k];
      if (!total || args[k] > g_max[index][k].load(std::memory_order_relaxed)) g_max[index][k] = args[k];
    }
    if (!total) g_caller[index] = uint32_t(ctx.lr);
  }
  int32_t at = -1;
  if (g_logging.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_mutex);
    if (g_logged[index]++ < kPerFunctionLogLimit) {
      at = int32_t(g_records.size());
      g_records.push_back({index, t_depth, {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32},
                           uint32_t(ctx.lr), 0, false});
    }
  }
  t_open.push_back(at);
  ++t_depth;
}

void Leave(uint32_t index, const PPCContext& ctx) {
  if (index >= kMaxFunctions) return;
  if (t_depth) --t_depth;
  if (t_open.empty()) return;
  const int32_t at = t_open.back();
  t_open.pop_back();
  if (at < 0 || !g_logging.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_mutex);
  if (size_t(at) < g_records.size() && g_records[at].index == index) {
    g_records[at].result = ctx.r3.u32;
    g_records[at].returned = true;
  }
}

}  // namespace xmd::d3d_trace

// Present: the frame boundary.
REX_EXTERN(__imp__sub_82738208);
REX_HOOK_RAW(sub_82738208) {
  using namespace xmd::d3d_trace;
  static uint64_t frame = 0;
  static const std::string dir = Dir();
  __imp__sub_82738208(ctx, base);
  ++frame;
  if (dir.empty()) return;
  if (g_logging.exchange(false)) WriteFrame(frame - 1);
  if (frame % 300 == 0) WriteCounts(frame);
  if (frame % 15 == 0) {
    std::error_code ec;
    const std::filesystem::path snap = std::filesystem::path(dir) / "snap";
    if (std::filesystem::exists(snap, ec)) {
      std::filesystem::remove(snap, ec);
      std::lock_guard lock(g_mutex);
      g_records.clear();
      g_records.reserve(200000);
      for (uint32_t& n : g_logged) n = 0;
      g_logging = true;
    }
  }
}
