# Mushihimesama Futari Ver 1.5 (Xbox 360): static recompilation host application.
#
# The game code is not in this repository: run the code generator first (tools/codegen.ps1), which writes generated/
# from your own default.xex together with the CMake glue (generated/rexglue.cmake, made by `rexglue codegen`).
#
# Stage 1, Windows PC:
#   cmake --preset win-pc && cmake --build --preset win-pc        (from app/, inside tools\win\env.ps1)
# Stage 2, Nintendo Switch (devkitA64), later:
#   cmake -S app -B out/nx -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake -DREXGLUE_SWITCH_NVK_SDK=<mesa sdk>

cmake_minimum_required(VERSION 3.25)
project(futari LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "app/generated/rexglue.cmake is missing: run tools/codegen.ps1 first")
endif()
include(generated/rexglue.cmake)

# The build re-runs codegen whenever one of its inputs changes (the manifest, an overrides toml, the xex). The
# post-codegen performance passes (tools/perf_codegen_post.py: direct calls, guest memory without volatile) have to
# follow EVERY codegen, or they are silently lost: that happened on 2026-10-06 (the first perf build regenerated the
# code at its start and dropped both). They are appended to the codegen command itself. FUTARI_PERF_CODEGEN_POST=OFF
# builds the generated code as rexglue writes it (the A side of the A/B pair).
option(FUTARI_PERF_CODEGEN_POST "Run tools/perf_codegen_post.py after every codegen the build runs" ON)
# A Direct3D census build (tools/d3d_census_build.py): the folder with its generated hooks (census_default.cpp for the
# executable, census_core.cpp for every game core) and hooks.txt, the functions they hook, which the post pass must
# keep calling through their hooks. Empty for every normal build.
set(FUTARI_CENSUS_DIR "" CACHE PATH "Direct3D census build: folder written by tools/d3d_census_build.py (empty = off)")
set(_futari_post_args "")
if(FUTARI_CENSUS_DIR)
    set(_futari_post_args --hooked-list "${FUTARI_CENSUS_DIR}/hooks.txt")
endif()
if(TARGET futari_codegen AND FUTARI_PERF_CODEGEN_POST)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp" APPEND
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/perf_codegen_post.py" ${_futari_post_args})
endif()

set(FUTARI_SOURCES
    src/main.cpp
    # PGO profile collection (the Switch build with FUTARI_PGO=generate; empty otherwise).
    src/futari_pgo.cpp
    # Video recordings of the game image for testing (cvar futari_video_s; tools/make_video.sh makes the MP4).
    src/futari_capture.cpp
    # DLC: installs Black Label / the Ver 1.01 key from dlc/ next to the executable (cvar futari_dlc_dir).
    src/futari_dlc.cpp
    # Windows: logs fatal host exceptions the SDK does not report (illegal instruction, ...) with module + offset.
    src/futari_crash_log.cpp
    # Scripted pad for unattended test runs (cvar futari_input_script; tools/win/boot_test.ps1 -Pad).
    src/futari_input_script.cpp
    # Fixes for races the Xbox 360's timing never exposed (thread start race seen on the Switch).
    src/futari_fixes.cpp
    # Memory over time for crashes that leave no report (cvar futari_memory_log_s; Switch: heap incl. GPU memory).
    src/futari_memory_log.cpp
    # On/off keys for every enhancement + the CPU / non-CPU masters (futari_cpu_enhancements, futari_noncpu_...).
    src/futari_enhancements.cpp
    # PC sampling profiler (cvar futari_pc_profile_s; tools/pc_hot_functions.py). Nothing on the Switch.
    src/futari_pc_profiler.cpp
    # Native replacements of hot guest functions (src/native/futari_native.h): framework + default.xex's natives. The
    # game cores' natives are compiled into each core target (below).
    src/native/futari_native.cpp
    src/native/futari_native_crt.cpp
    src/native/futari_native_frame.cpp
    # Localized copies of the hottest guest functions (tools/localize_native.py -> app/generated/hot).
    src/native/futari_native_hot.cpp
    # Stage 2: the native renderer (src/me, masseffect-nx's PM4-ring Vulkan renderer as adapted by banjo-nx). It
    # replaces the emulated Xenos GPU with futari_renderer_native = true; off by default until it draws Futari right.
    src/me/me_native_system.cpp
    src/me/me_ring_wait.cpp
    src/me/futari_direct_hooks.cpp
    # The Direct3D census (cvar futari_diag_d3d_census; its hooks only in a census build, FUTARI_CENSUS_DIR below).
    src/me/futari_d3d_census.cpp
    # The native Direct3D layer (futari_api_*: Direct3D's entry points as native code writing the ring's records).
    src/me/futari_api.cpp
    src/me/me_renderdoc.cpp
    src/me/futari/futari_native_draws.cpp
    src/me/futari/futari_deferred_recording.cpp
    src/me/futari/futari_native_targets.cpp
    src/me/futari/futari_native_shaders.cpp
    src/me/futari/futari_native_textures_pool.cpp
    src/me/futari/futari_shader_library.cpp
    src/me/futari/me_futari_glue.cpp
)
if(NOT REXGLUE_PLATFORM_SWITCH)
    # Switch profiler counters the native renderer calls (no-ops on PC).
    list(APPEND FUTARI_SOURCES src/me/me_pc_stubs.cpp)
endif()
if(FUTARI_CENSUS_DIR)
    list(APPEND FUTARI_SOURCES "${FUTARI_CENSUS_DIR}/census_default.cpp")
endif()

if(WIN32)
    add_executable(futari WIN32 ${FUTARI_SOURCES})
else()
    add_executable(futari ${FUTARI_SOURCES})
endif()

# Link the SDK. Stage 1 draws with ReXGlue's Xenos GPU emulation; the native renderer replaces it later.
rexglue_setup_target(futari GPU_PLUGINS xenos)

# SPIR-V initializers of the native renderer's EDRAM transfer / utility shaders (src/me/futari/shaders), compiled
# on the host with glslangValidator (tools/win/build_glslang.ps1 builds it from the SDK's glslang). The Switch build
# uses the same host tool: SPIR-V is platform independent.
set(FUTARI_GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/../out/glslang/StandAlone/glslangValidator${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE FILEPATH "Host glslangValidator")
set(FUTARI_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
set(FUTARI_EDRAM_SHADER_INCLUDES)
foreach(_shader futari_edram_7e3_to_rgba8 futari_edram_rgba8_to_7e3 futari_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 futari_resolve_copy)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader STREQUAL "futari_resolve_copy")
        set(_extension frag)
    elseif(_shader MATCHES "^me_edram_(color_to_depth|depth_to_depth|depth_to_stencil|color_to_stencil|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x|raw64_to_depth|color_to_color|r64_to_r64)$")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/me/futari/shaders/${_shader}.${_extension}")
    set(_output "${FUTARI_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader MATCHES "^me_edram_(stencil_to_buffer|color_stencil_to_buffer|stencil_msaa2_to_buffer|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x)$" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${FUTARI_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${FUTARI_GENERATED_SHADER_DIR}"
        COMMAND "${FUTARI_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND FUTARI_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(futari_edram_shaders DEPENDS ${FUTARI_EDRAM_SHADER_INCLUDES})
add_dependencies(futari futari_edram_shaders)
target_include_directories(futari PRIVATE
    "${FUTARI_GENERATED_SHADER_DIR}"
    src
    src/me
    src/me/futari
    src/me/futari/shaders
    "${REXSDK_DIR}/src/core"
    # Precompiled SPIR-V of the SDK presenter (vulkan_spirv/guest_output_*): the renderer draws into its output.
    "${REXSDK_DIR}/src/ui/shaders")

# CPU-only SDK sources the native renderer reuses (draw-extent estimation, shader microcode interpretation): the
# GPU plugin's private symbols cannot satisfy application-side use.
target_sources(futari PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

# The renderer includes xxhash.h and renderdoc_app.h directly; when the SDK is built from source they are private to
# its libraries.
foreach(_dep xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(futari PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# Settings template next to the executable, only when there is none yet (the copy there is the user's to edit).
if(NOT REXGLUE_PLATFORM_SWITCH)
    add_custom_command(TARGET futari POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/futari.toml -DDST=$<TARGET_FILE_DIR:futari>/futari.toml
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        VERBATIM)
endif()

# The SDK compiles itself with these (directory scope), but they do not reach the generated game code, which is
# built in this directory. GCC's default for GNU C++ is -ffp-contract=fast: it fuses separate PPC fmul/fadd into
# FMA and results drift from the Xbox 360. Strict aliasing is unsafe for recompiled code (guest memory and the
# context are accessed through every type).
# The eight game cores are targets of their own (futari_<core>): they were missing here until 2026-10-06 (banjo-nx
# found the same gap), so the Switch build compiled all gameplay code with -ffp-contract=fast and strict aliasing.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target futari futari_recomp
            futari_palm_normal futari_palm_abnormal futari_reco_normal futari_reco_abnormal
            futari_hirow_normal futari_hirow_abnormal futari_kiniro_normal futari_kiniro_abnormal)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
endif()

# 2026-10-07 (masseffect-nx ships it; sasrt-nx's reminder): no errno after sqrt & co. (one fsqrt instead of a check and
# a possible libm call) and no assumption that a floating-point operation can trap. IEEE results are unchanged (no
# -ffinite-math-only: NaN and infinity checks stay); only errno and trap semantics, which the recompiled code never
# observes. Build-time switch: -DFUTARI_GENERATED_MATH_FLAGS=OFF builds without them (the A/B is a pair of builds).
option(FUTARI_GENERATED_MATH_FLAGS "-fno-math-errno -fno-trapping-math on the generated game code" ON)
if(FUTARI_GENERATED_MATH_FLAGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target futari futari_recomp
            futari_palm_normal futari_palm_abnormal futari_reco_normal futari_reco_abnormal
            futari_hirow_normal futari_hirow_abnormal futari_kiniro_normal futari_kiniro_abnormal)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -fno-math-errno -fno-trapping-math)
        endif()
    endforeach()
endif()

# Native replacements inside the game cores (src/native/futari_native_core.cpp): a core's symbols are local to it, so
# its natives are compiled into its own target, told which core it is by FUTARI_CORE_<name>. Not built with the
# generated code's precompiled header.
foreach(_core palm_normal palm_abnormal reco_normal reco_abnormal hirow_normal hirow_abnormal kiniro_normal kiniro_abnormal)
    if(TARGET futari_${_core})
        target_sources(futari_${_core} PRIVATE src/native/futari_native_core.cpp)
        target_compile_definitions(futari_${_core} PRIVATE FUTARI_CORE_${_core})
        if(FUTARI_CENSUS_DIR)
            target_sources(futari_${_core} PRIVATE "${FUTARI_CENSUS_DIR}/census_core.cpp")
            set_source_files_properties("${FUTARI_CENSUS_DIR}/census_core.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endif()
endforeach()
set_source_files_properties(src/native/futari_native_core.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(REXGLUE_PLATFORM_SWITCH)
    # Link map: which object (game core) each function of the profile comes from (tools/switch_hot_functions.py).
    target_link_options(futari PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/futari.map")
    # The Switch build's fixed settings, compiled in (src/futari_app.h BuiltInConfig): everything futari.toml no longer
    # lists (sa2-nx's design). The SD card's futari.toml holds the player's options and overrides these key by key.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/futari_switch_defaults.toml" FUTARI_BUILTIN_CONFIG)
    # And the player's futari.toml, written next to the NRO when it is missing (src/futari_app.h DefaultConfigFile).
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/futari_switch.toml" FUTARI_PLAYER_CONFIG)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/futari_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/futari_builtin_config.h" @ONLY)
    target_include_directories(futari PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(futari PRIVATE FUTARI_BUILTIN_CONFIG=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/futari_switch_defaults.toml" "${CMAKE_CURRENT_SOURCE_DIR}/futari_switch.toml")
    # Switch compiler optimisations (2026-10-08, the user's request: sa2-nx's set, from nfsmw-nx / UnleashedRecomp-NX):
    #   - LTO: the recompiled default.xex code (futari_recomp) and the app (renderer, natives) are compiled to GIMPLE
    #     (-flto -fno-fat-lto-objects) and optimised as one program at the link (-flto=N -flto-partition=balanced: N
    #     parallel LTRANS jobs, kept low for the build machine's memory). The SDK libraries and the eight game cores
    #     (partially linked with ld -r + objcopy, which needs machine code) stay out.
    #   - -fipa-pta: interprocedural points-to analysis at the LTO link.
    #   - The link repeats -ffp-contract=off and -fno-strict-aliasing (the LTRANS stage must not fuse what the compile
    #     stage kept apart), and xxHash is built with XXH_FORCE_MEMORY_ACCESS=0: nfsmw-nx's LTO builds hit xxHash's
    #     default (a strict-aliasing violation) once it was inlined into a cache-key function (garbage keys).
    #   - Hot functions first in .text (app/hot_functions.ld, tools/hot_order.py from console stack samples). It
    #     starts with KEEP(*(.crt0)): the start-up code must stay at address 0 (tools/check_switch_build.py checks it).
    option(FUTARI_SWITCH_LTO "Link-time optimisation of the default.xex code and the app (Switch)" ON)
    set(FUTARI_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    option(FUTARI_SWITCH_IPA_PTA "-fipa-pta at the Switch LTO link" ON)
    option(FUTARI_SWITCH_HOT_ORDER "Hot functions first in .text (app/hot_functions.ld)" ON)
    target_compile_definitions(futari PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(FUTARI_SWITCH_LTO)
        set(_futari_lto_compile -flto -fno-fat-lto-objects)
        set(_futari_lto_link -flto=${FUTARI_SWITCH_LTO_JOBS} -flto-partition=balanced -ffp-contract=off
                             -fno-strict-aliasing)
        if(FUTARI_SWITCH_IPA_PTA)
            list(APPEND _futari_lto_compile -fipa-pta)
            list(APPEND _futari_lto_link -fipa-pta)
        endif()
        foreach(target futari_recomp futari)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_futari_lto_compile})
            endif()
        endforeach()
        target_link_options(futari PRIVATE ${_futari_lto_link})
        message(STATUS "futari: Switch LTO on (${FUTARI_SWITCH_LTO_JOBS} LTRANS jobs, -fipa-pta ${FUTARI_SWITCH_IPA_PTA})")
    endif()
    # Line tables for the app's own code: console profiles map to source lines with addr2line on the archived ELF. -g1
    # does not change the generated code; the NRO is made from the stripped ELF. The generated game code stays without.
    option(FUTARI_SWITCH_LINE_INFO "Line tables (-g1) for the app's own code in the Switch ELF" ON)
    if(FUTARI_SWITCH_LINE_INFO)
        # DWARF 4: with DWARF 5 sa2-nx's LTO output mapped some functions' lines to the wrong file.
        target_compile_options(futari PRIVATE -g1 -gdwarf-4)
        target_link_options(futari PRIVATE -g1 -gdwarf-4)
    endif()
    if(FUTARI_SWITCH_HOT_ORDER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.ld")
        target_link_options(futari PRIVATE "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.ld")
        set_property(TARGET futari APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.ld")
    endif()
    # Profile-guided optimisation (2026-10-08, the user's request; sa2-nx's design), for the generated default.xex code
    # and the app (the eight game cores, partially linked with ld -r + objcopy, stay out as sa2-nx's module does):
    #   FUTARI_PGO=generate  an instrumented NRO that writes its branch counts to pgo/ on the SD card
    #                        (src/futari_pgo.cpp; registered in a section, not by constructors: libgcov's exit-time
    #                        files do not fit the Switch). Value profiling is off: its shared lists are not safe without
    #                        atomic updates.
    #   FUTARI_PGO=use       the counts merged on the PC (tools/win/pgo_merge.ps1). Code the sessions did not run is
    #                        compiled as without a profile (-fprofile-partial-training), and functions changed since
    #                        the instrumented build keep no profile (-Wno-coverage-mismatch).
    # The profile is pgo/ at the root of the repository (FUTARI_PGO_DIR): one .gcda per object, named by the object's
    # path in the build folder with # for each folder separator (-fprofile-prefix-path, the build folder written the
    # Windows way, as GCC sees it).
    set(FUTARI_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE FUTARI_PGO PROPERTY STRINGS "" generate use)
    set(FUTARI_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
    get_filename_component(_futari_pgo_dir "${FUTARI_PGO_DIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_futari_pgo_dir}" _futari_pgo_dir)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _futari_pgo_prefix)
    if(FUTARI_PGO STREQUAL "generate")
        set(_futari_pgo "-fprofile-generate=${_futari_pgo_dir}" "-fprofile-prefix-path=${_futari_pgo_prefix}"
                        -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(futari PRIVATE FUTARI_PGO_GENERATE=1)
    elseif(FUTARI_PGO STREQUAL "use")
        if(NOT EXISTS "${FUTARI_PGO_DIR}")
            message(FATAL_ERROR "FUTARI_PGO=use: no profile in ${FUTARI_PGO_DIR}")
        endif()
        # -fprofile-correction: counts taken without atomic updates (-fprofile-update=single) while several game
        # threads ran the same code lose a few increments; GCC smooths them instead of stopping.
        set(_futari_pgo "-fprofile-use=${_futari_pgo_dir}" "-fprofile-prefix-path=${_futari_pgo_prefix}"
                        -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile
                        -Wno-coverage-mismatch)
    elseif(FUTARI_PGO)
        message(FATAL_ERROR "FUTARI_PGO must be empty, generate or use (got ${FUTARI_PGO})")
    endif()
    if(FUTARI_PGO)
        foreach(target futari_recomp futari)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_futari_pgo})
            endif()
        endforeach()
        target_link_options(futari PRIVATE ${_futari_pgo})
        message(STATUS "futari: PGO ${FUTARI_PGO}")
    endif()
    # The homebrew menu's entry: name, author, version and icon (app/futari_icon.jpg, 256x256 JPEG from the cover).
    rexglue_switch_add_nro(futari
        NAME "Mushihimesama Futari"
        AUTHOR "ChanseyIsTheBest"
        VERSION "1.0.0"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/futari_icon.jpg")
endif()
