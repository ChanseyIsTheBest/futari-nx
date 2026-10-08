# Patches

Every change this port makes to third-party code, as diffs against the upstream sources it was taken from. The changed
files are also in the repository as they are, so it builds without applying anything: these patches show exactly
what changed, and let you apply the same changes to a fresh upstream checkout.

| Patch | Upstream base | What it changes |
|---|---|---|
| `rexglue-sdk-v0.10.0.patch` | [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) v0.10.0, commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` | The whole SDK fork (`sdk/`): the Horizon (Switch) layer, the code generator's options, the console tuning of the runtime, and the SDK's build of FFmpeg (`thirdparty/CMakeLists.txt`, `thirdparty/ffmpeg-overlay/codec_list.c`). 183 files, +22,034 / -771 lines |
| `ffmpeg-0604b46.patch` | [wmarti/FFmpeg](https://github.com/wmarti/FFmpeg) commit `0604b464c7cb4ebc94940cf1f324a3b26b87717c`, the FFmpeg submodule of rexglue-sdk v0.10.0 | `config.h` (the Switch configuration), `libavcodec/fft_template.c` and `libavcodec/mdct_template.c` (a table cache), and two new files: `config_switch_aarch64.h` and `config_masseffect_wmv3.h` |
| `xenosrecomp-990d03b.patch` | [hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp) commit `990d03b28a27b50277ee5d8d942e1c5f873869d1` | The shader translator (`shaders/XenosRecomp`): `shader_recompiler.cpp`/`.h` and `shader_common.h`, and the new `pch_min.h`. What each change does is in `shaders/README.md` |

The NVK driver (Mesa) is not changed by this port: see [mesa/README.md](../mesa/README.md).

## Applying them

Each patch is a `git diff` from the root of its upstream repository:

```sh
git clone https://github.com/rexglue/rexglue-sdk.git && cd rexglue-sdk
git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
git apply ../futari-nx/patches/rexglue-sdk-v0.10.0.patch

git submodule update --init thirdparty/FFmpeg && cd thirdparty/FFmpeg
git apply ../../../futari-nx/patches/ffmpeg-0604b46.patch

git clone https://github.com/hedge-dev/XenosRecomp.git && cd XenosRecomp
git checkout 990d03b28a27b50277ee5d8d942e1c5f873869d1
git apply ../futari-nx/patches/xenosrecomp-990d03b.patch
```

The SDK patch leaves upstream's folders that the fork does not carry (`docs/`, `scripts/`, `tests/`, `tools/`,
`.github/` and the editor settings) as they are; nothing in the port's build uses them. Line endings are LF, as git
stores the files.

`tools/make_patches.py` makes them (it lays this repository's files over a clean clone of each base and diffs), and
checks each one with `git apply --check` on another clean clone.
