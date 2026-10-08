// futari - native renderer: access to the shader library the native graphics system has loaded.

#pragma once

namespace futari::renderer {

class ShadersNative;

// The library the native graphics system loaded (nullptr until then), from any thread. Used for pipeline
// prewarming (futari_native_draws.cpp).
const ShadersNative* ActiveLibrary();

}  // namespace futari::renderer
