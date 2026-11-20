// X-Men: Destiny - no free camera in this port (the renderer asks whether the HUD is hidden).
#pragma once

namespace xmd {

inline bool UiHidden() { return false; }

}  // namespace xmd