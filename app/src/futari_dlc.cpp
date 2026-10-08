// Downloadable content (see futari_dlc.h).
//
// Mushihimesama Futari Ver 1.5 has two marketplace packages (content type 0x00000002):
//   - Black Label: Rom/cg_black.bin, Rom/cg_blackhi.bin, Rom/dropprm.bl, spa.bin
//   - the Ver 1.01 unlock: ver101_key.bin
// The game's DLCControl enumerates them with XamContentCreateEnumerator and mounts each one (log line
// "DLCControl : DLCID=%d mounted as ..."). Copy your own packages into dlc/ next to the executable, either as package
// files or unpacked (dlc/00000002/<package name>/...): on the next start they are put into the content store (user
// data folder, 0000000000000000/435A07D7/00000002/<package name>/) with a .header. Packages already there are skipped.
//
// DLCControl recognises a package by its DISPLAY NAME, not by its file name or contents: the state after XEnumerate
// (sub_82192B08 in default.xex) compares XCONTENT_DATA.szDisplayName (+8) with wcscmp (sub_8207AE18) against the name
// of the DLCID it looks for, taken from the table at 0x821EC988 (sub_82193C28 copies it):
//   DLCID 0 = 0x82046958 L"虫姫さまふたり　ブラックレーベル" (Black Label), DLCID 1 = 0x8204697C L"虫姫さまふたり　Ver1.01".
// An STFS package carries that name in its header. An unpacked package has none, and the content store then lists it
// under its folder name, so the game never finds it and greys Black Label out in the TOP MENU. The display name of an
// unpacked package is therefore taken from its files (Rom/cg_black.bin: Black Label, ver101_key.bin: Ver 1.01), both
// when it is installed and, for packages installed before this fix, by rewriting the .header at every start.

#include "futari_dlc.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

REXCVAR_DEFINE_STRING(futari_dlc_dir, "", "Futari",
                      "Folder with Xbox 360 DLC to install at startup (empty = dlc/ next to the executable): package "
                      "files, or unpacked packages in <folder>/00000002/<package name>/");

namespace futari::dlc {
namespace {

// STFS packages start with "LIVE", "PIRS" or "CON ".
bool IsContentPackage(const std::filesystem::path& path) {
  char magic[4] = {};
  std::FILE* f = rex::filesystem::OpenFile(path, "rb");
  if (!f) {
    return false;
  }
  const bool read = std::fread(magic, 1, 4, f) == 4;
  std::fclose(f);
  const std::string m(magic, 4);
  return read && (m == "LIVE" || m == "PIRS" || m == "CON ");
}

// Package names are the 40-42 hex digits of the package file name.
bool LooksLikePackageName(const std::string& name) {
  if (name.size() < 40 || name.size() > 42) {
    return false;
  }
  for (char c : name) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// The display names default.xex looks for (UTF-16 in the image, see the top of this file).
constexpr std::u16string_view kBlackLabelName =
    u"\u866B\u59EB\u3055\u307E\u3075\u305F\u308A\u3000\u30D6\u30E9\u30C3\u30AF\u30EC\u30FC\u30D9\u30EB";
constexpr std::u16string_view kVer101Name = u"\u866B\u59EB\u3055\u307E\u3075\u305F\u308A\u3000Ver1.01";

// Which of the two packages an unpacked folder is, from its files; empty when it is neither.
std::u16string_view DisplayNameFromFiles(const std::filesystem::path& package) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(package / "Rom" / "cg_black.bin", ec) ||
      std::filesystem::is_regular_file(package / "spa.bin", ec)) {
    return kBlackLabelName;
  }
  if (std::filesystem::is_regular_file(package / "ver101_key.bin", ec)) {
    return kVer101Name;
  }
  return {};
}

// Gives every installed package of this title that is one of the two DLC the display name the game compares against.
// Packages installed from STFS files already have it; unpacked ones installed before this fix carry their folder name.
void RepairDisplayNames(rex::Runtime* runtime, rex::system::KernelState* kernel,
                        rex::system::xam::ContentManager* content) {
  const auto root = runtime->user_data_root() / "0000000000000000" / fmt::format("{:08X}", kernel->title_id()) /
                    fmt::format("{:08X}", uint32_t(rex::system::XContentType::kMarketplaceContent));
  auto installed = content->ListContent(uint32_t(rex::system::xam::DummyDeviceId::HDD), 0,
                                        rex::system::XContentType::kMarketplaceContent, kernel->title_id());
  for (auto& item : installed) {
    const auto wanted = DisplayNameFromFiles(root / item.file_name());
    if (wanted.empty() || item.display_name() == wanted) {
      continue;
    }
    item.set_display_name(wanted);
    // Unpacked packages are installed with license mask 0xFFFFFFFF (all offers licensed), as on first install.
    const auto result = content->WriteContentHeaderFile(0, item, 0xFFFFFFFF);
    if (result == 0) {
      REXLOG_INFO("[dlc] {}: display name set to the one the game looks for ({})", item.file_name(),
                  wanted == kBlackLabelName ? "Black Label" : "Ver 1.01");
    } else {
      REXLOG_WARN("[dlc] {}: could not rewrite the header: {:08X}", item.file_name(), result);
    }
  }
}

}  // namespace

void InstallPackages(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content) {
    return;
  }
  std::string dir = REXCVAR_GET(futari_dlc_dir);
  std::erase(dir, '"');  // string cvars can come back quoted
  const std::filesystem::path folder =
      dir.empty() ? rex::filesystem::GetExecutableFolder() / "dlc" : std::filesystem::path(dir);
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) {
    RepairDisplayNames(runtime, kernel, content);
    return;
  }
  const auto installed = content->ListContent(uint32_t(rex::system::xam::DummyDeviceId::HDD), 0,
                                              rex::system::XContentType::kMarketplaceContent,
                                              kernel->title_id());
  auto is_installed = [&](const std::string& name) {
    for (const auto& item : installed) {
      if (item.file_name() == name) {
        return true;
      }
    }
    return false;
  };
  auto install_dir = [&](const std::filesystem::path& package) {
    const std::string name = rex::path_to_utf8(package.filename());
    if (is_installed(name)) {
      REXLOG_INFO("[dlc] {} already installed", name);
      return;
    }
    const auto result = content->InstallUnpackedContent(package, DisplayNameFromFiles(package));
    if (result == 0) {
      REXLOG_INFO("[dlc] installed unpacked {}", name);
    } else {
      REXLOG_WARN("[dlc] could not install unpacked {}: {:08X}", name, result);
    }
  };

  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    const std::string name = rex::path_to_utf8(entry.path().filename());
    if (entry.is_directory(ec)) {
      if (LooksLikePackageName(name)) {
        install_dir(entry.path());
      } else if (name == "00000002") {
        for (const auto& sub : std::filesystem::directory_iterator(entry.path(), ec)) {
          if (sub.is_directory(ec) && LooksLikePackageName(rex::path_to_utf8(sub.path().filename()))) {
            install_dir(sub.path());
          }
        }
      }
      continue;
    }
    if (!entry.is_regular_file(ec) || !IsContentPackage(entry.path())) {
      continue;
    }
    if (is_installed(name)) {
      REXLOG_INFO("[dlc] {} already installed", name);
      continue;
    }
    const auto result = content->InstallContent(entry.path());
    if (result == 0) {
      REXLOG_INFO("[dlc] installed {}", name);
    } else {
      REXLOG_WARN("[dlc] could not install {}: {:08X}", name, result);
    }
  }
  RepairDisplayNames(runtime, kernel, content);
}

}  // namespace futari::dlc
