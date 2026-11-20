// Test aid (profile_sampler.cpp): the render thread's time by function.
#pragma once

namespace xmd::profile {
// Called at each present by the thread that presents (the first call starts the sampler when
// XMD_PROFILE_TRIGGER is set; without it, nothing).
void FrameThread();
}  // namespace xmd::profile
