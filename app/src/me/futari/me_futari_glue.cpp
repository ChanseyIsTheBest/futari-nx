// Mass Effect glue for the native renderer modules: "feature off" answers for the hooks they call that
// have no Mass Effect implementation (deferred-recording reports) and the shader library handle.
#include <string>

#include <rex/logging.h>

#include "futari_native_hooks.h"
#include "futari_native_shaders.h"

namespace futari::renderer {
void DeferredReport(std::string line) { REXLOG_INFO("{}", line); }

// Set by the native graphics system once the shader library is loaded.
const ShadersNative* g_active_library = nullptr;
const ShadersNative* ActiveLibrary() { return g_active_library; }
}  // namespace futari::renderer
