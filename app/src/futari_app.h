// Mushihimesama Futari: the ReXGlue application object (paths, GPU plugin choice, DLC, analysis aids).

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "futari_capture.h"
#include "futari_dlc.h"
#include "futari_enhancements.h"
#include "futari_input_script.h"
#include "futari_memory_log.h"
#include "futari_pgo.h"
#include "futari_pc_profiler.h"
#include "me/me_native_system.h"
// Renut's Direct3D-level renderer (futari_renderer = "renut"; cmake/futari_renut_gpu.cmake).
#if FUTARI_RENUT_RENDERER
#include <rex/frame_latency.h>
#include <rex/ui/immediate_drawer.h>
#include "renut_engine/gpu/banjo_renut_capture.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/futari_renut_vblank.h"
#endif
#if defined(FUTARI_BUILTIN_CONFIG)
#include "futari_builtin_config.h"  // generated from app/futari_switch_defaults.toml and futari_switch.toml (CMake)
#endif

class FutariApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FutariApp>(new FutariApp(ctx, "futari", PPCImageConfig));
  }

  // The Switch build's fixed settings (app/futari_switch_defaults.toml, compiled in): applied before futari.toml,
  // which holds only the player's options (and may still override any of these).
  std::string_view BuiltInConfig() const override {
#if defined(FUTARI_BUILTIN_CONFIG)
    return kFutariBuiltInConfig;
#else
    return {};
#endif
  }
  // Without a futari.toml next to the NRO, the player's default one is written there (app/futari_switch.toml).
  std::string_view DefaultConfigFile() const override {
#if defined(FUTARI_BUILTIN_CONFIG)
    return kFutariPlayerConfig;
#else
    return {};
#endif
  }

  void OnPostInitLogging() override {
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation, keeping an explicit
    // command-line or config-file choice.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
#if REX_PLATFORM_SWITCH
    // The emulated Xenos GPU runs at a few frames per second on the console (nfsmw-nx, banjo-nx): the native renderer
    // is the default there. An explicit futari_renderer_native in futari.toml still wins.
    if (!rex::cvar::HasNonDefaultValue("futari_renderer_native")) {
      rex::cvar::SetFlagByName("futari_renderer_native", "true");
    }
#endif
    // Enhancement masters (futari_cpu_enhancements / futari_noncpu_enhancements) and the "[enhancements]" log line,
    // before anything reads the keys.
    futari::enhancements::Apply();
  }

  // Without --game_data_root, use a game_root/ folder next to the executable (on the Switch:
  // sdmc:/switch/futari-nx/game_root), and on a PC build tree the repository's assets/game_root.
  // An explicit path always wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
#if REX_PLATFORM_SWITCH
    // A loader that gives the NRO no path (Ryujinx's homebrew loader: the executable folder is the SD card's root):
    // the release's own folder, then the SD card's root.
    for (const char* fixed : {"/switch/futari-nx/game_root", "/game_root"}) {  // rooted: "sdmc:/..." is relative to libstdc++
      if (std::filesystem::is_regular_file(std::filesystem::path(fixed) / "default.xex", ec)) {
        const auto exe = rex::filesystem::GetExecutableFolder();
        if (exe.empty() || !std::filesystem::is_regular_file(exe / "game_root" / "default.xex", ec)) {
          paths.game_data_root = fixed;
          return;
        }
        break;
      }
    }
#endif
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      for (const auto& sub : {std::filesystem::path("game_root"),
                              std::filesystem::path("assets") / "game_root"}) {
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

  // Stage 2: with futari_renderer_native = true the graphics system is the app's own native Vulkan renderer
  // (src/me), and the emulated Xenos GPU plugin is not loaded (ReXApp only loads it when config.graphics is empty).
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if FUTARI_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      // No SDK graphics system and no GPU plugin: the renderer is the app's own (the SDK's detached mode).
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("graphics: Renut's Direct3D-level renderer (plume; no PM4 ring, no EDRAM model)");
      return;
    }
#endif
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
      REXLOG_INFO("graphics: native Vulkan renderer (PM4 ring)");
    }
  }

  void OnPostSetup() override {
    // DLC (Black Label, Ver 1.01) from dlc/ next to the executable goes into the content store before the game's
    // DLCControl looks for it.
    futari::dlc::InstallPackages(runtime());
    // Unattended runs: a scripted pad (futari_input_script) to get through the title screen and menus.
    futari::input_script::Install(runtime());
    // Video recording of the game image for testing (futari_video_s = N seconds after start; 0 = off). Taken from
    // the presenter's guest output, so it works minimized and nothing can cover it.
#if FUTARI_RENUT_RENDERER
    // Renut's renderer has no GPU: the guest's vertical blank comes from a thread of its own (futari_renut_vblank.h).
    if (banjo::renut_gpu::Active()) futari::renut_vblank::Start();
    // Renut's renderer has no SDK presenter: its own Present reads the frame back (banjo_renut_capture.h).
    if (banjo::renut_gpu::Active()) {
      futari::capture::Start([](rex::ui::RawImage& image) { return renut::gpu::CaptureFrame(image, 100); },
                             [] { return rex::latency::SwapCount(); });
    } else
#endif
    futari::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
    // Memory over time (futari_memory_log_s = N seconds; 0 = off).
    futari::memory_log::Start();
    // PC: function-level CPU profile (futari_pc_profile_s = N seconds per report; 0 = off).
    futari::pc_profiler::Start();
    // The PGO-instrumented build (FUTARI_PGO=generate) saves its counts to pgo/ every 2 minutes (nothing otherwise).
    futari::pgo::Start();
  }

  // The PGO-instrumented build saves its counts when the game loses focus (HOME menu): it may be closed from there.
  void OnWindowFocusChanged(bool focused) override {
    if (!focused) futari::pgo::DumpNow();
  }

  void OnShutdown() override {
    futari::pgo::Stop();
    futari::pc_profiler::Stop();
    futari::memory_log::Stop();
    futari::capture::Stop();
#if FUTARI_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      futari::renut_vblank::Stop();
      renut::gpu::ShutdownRenderer();
    }
#endif
  }

#if FUTARI_RENUT_RENDERER
  // Renut's renderer draws the SDK's ImGui through its own plume drawer, and resizes its swapchain itself.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return rex::ReXApp::OnCreateImmediateDrawer();
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
  // The window exists here (UI thread, before the game starts): Renut's renderer comes up first.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active()) {
      renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    }
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
#endif

  // Analysis aid: with FUTARI_DUMP_IMAGE=<file>, write the loaded guest image (image_base..image_base+image_size) so
  // code pointers stored in data can be scanned for (tools/pointer_scan.py). Does nothing unless the variable is set.
  void OnPostLoadXexImage() override {
    if (!runtime() || !runtime()->memory()) {
      return;
    }
    const char* path = std::getenv("FUTARI_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
      REXLOG_INFO("guest image written to {}", path);
    }
  }
};
