futari-nx Switch test build
===========================

SD card layout (tools\win\package_switch.ps1 builds it in out\switch_sd\; copy that folder's contents to the SD root):
  sdmc:/switch/futari-nx/futari.nro
  sdmc:/switch/futari-nx/futari.toml                 settings (native renderer on)
  sdmc:/switch/futari-nx/futari_shaders.mesp         shader package for the native renderer
  sdmc:/switch/futari-nx/futari_shaders.mesp.idx
  sdmc:/switch/futari-nx/game_root/                  your extracted disc: default.xex, media\ (AvatarAssetPack,
                                                     nxeart and $SystemUpdate are not needed)
  sdmc:/switch/futari-nx/dlc/00000002/<package>/     optional: the unpacked Black Label / Ver 1.01 packages

Launch with a title takeover (hold R while starting a game, then open the Homebrew Menu). From the Album the port does
not get enough memory.

This build draws with the native Vulkan renderer (NVK), the same one that is pixel-identical to the emulated GPU on the
PC. The eight game cores (media/Module/*.xex) are linked into the NRO; the log line
  "Linked module 'reco_normal.xex' (futari_reco_normal) wired"
shows one being used when a game starts.

After a run, please copy back:
  sdmc:/switch/futari-nx/logs/          (the whole folder)
  sdmc:/atmosphere/crash_reports/       (only if it crashed: the newest file)

To compare against the emulated GPU (slow, a few FPS, but the reference image), set
  futari_renderer_native = false
  gpu_plugin = "xenos"
in futari.toml.
