// X-Men: Destiny - which graphics API to draw with. The renderer reports a
// lost device here; this port has no automatic fallback yet.
#pragma once

#include <string>

namespace xmd {

inline std::string AvoidPickedGraphicsApi(const std::string&) { return {}; }

}  // namespace xmd