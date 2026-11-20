// xmd - X-Men: Destiny, ReXGlue recompiled project

#include "generated/default/xmd_init.h"

#include "xmd_app.h"

REX_DEFINE_APP(xmd, XmdApp::Create)

// Laptops with two GPUs: ask NVIDIA Optimus and AMD switchable graphics for
// the fast one. The drivers only read these from the program itself.
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
