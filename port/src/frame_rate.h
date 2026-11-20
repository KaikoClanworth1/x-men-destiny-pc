// X-Men: Destiny - frame rate hooks the native renderer calls.
// (Nothing behind them yet: the game runs at its own 30 frames a second.)
#pragma once

namespace xmd {

inline int FrameRateNow() { return 30; }
// Test aids of the renderer's backends (publish-to-screen latency): not built here.
inline void LatencyOnPublish() {}
inline void LatencyOnSwap() {}

}  // namespace xmd