// Downloadable content: installs the Xbox 360 content the game can use (Black Label, the Ver 1.01 key) from a dlc/
// folder next to the executable into the runtime's content store, before the game starts, so the game finds it the
// way the console would.

#pragma once

namespace rex {
class Runtime;
}

namespace futari::dlc {

// Installs everything in <exe folder>/dlc (or the folder in futari_dlc_dir) that is not installed yet: STFS package
// files, and unpacked packages (a folder per package, named like the package file, directly in dlc/ or in a
// dlc/00000002/ content-type folder).
void InstallPackages(rex::Runtime* runtime);

}  // namespace futari::dlc
