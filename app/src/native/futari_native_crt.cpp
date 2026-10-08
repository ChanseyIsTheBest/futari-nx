// Native replacements of default.xex's C runtime functions (futari_native_crt.h). The game cores' copies are in
// futari_native_core.cpp.

#include "futari_native_crt.h"

#include <rex/hook.h>

extern "C" REX_FUNC(__imp__sub_8207DAC0);
extern "C" REX_FUNC(__imp__sub_8207DA20);

REX_HOOK_RAW(sub_8207DA20) {
  static futari::native::Site site{"futari_native_memset", "memset", 0x8207DA20, futari::native::crt::kMemsetWords,
                                   futari::native::crt::kMemsetHash};
  futari::native::crt::MemsetSite<__imp__sub_8207DA20>(site, ctx, base);
}

REX_HOOK_RAW(sub_8207DAC0) {
  static futari::native::Site site{"futari_native_memcpy", "memcpy", 0x8207DAC0, futari::native::crt::kMemcpyWords,
                                   futari::native::crt::kMemcpyHash};
  futari::native::crt::MemcpySite<__imp__sub_8207DAC0>(site, ctx, base);
}
