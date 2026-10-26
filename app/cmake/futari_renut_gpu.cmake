# futari-nx (2026-10-19): Renut's Direct3D-level renderer (github.com/masterspike52/reNut by reNut and masterspike52,
# through banjo-nx's adaptation: app/src/renut_engine/gpu) as Futari's third renderer, chosen at boot with
# futari_renderer = "renut" (src/renut_engine/gpu/banjo_renut_select.h). No PM4 ring and no EDRAM model: Futari's
# Direct3D runs natively through plume (Vulkan on the PC, deko3d on the Switch). Adapted from banjo-nx's
# cmake/banjo_renut_gpu.cmake; its hooks name Futari's Direct3D functions (src/renut_engine/futari_d3d_functions.txt,
# tools/renut_futari_hooks.py writes the sites in default.xex and the eight cores).
#
# An OBJECT library, not a static one: its hooks replace the generated code's weak sub_X definitions, and a static
# archive's members are only pulled in for undefined symbols, so the hooks would silently not be linked.

if(FUTARI_CENSUS_DIR)
    message(FATAL_ERROR "FUTARI_RENUT_RENDERER and a Direct3D census build (FUTARI_CENSUS_DIR) hook the same functions: "
                        "configure the census build with -DFUTARI_RENUT_RENDERER=OFF")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")

# The shader containers the shader cache is built from: tools/renut_dump_shaders.py's dump of default.xex and the
# eight cores (out/renut_shader_dump; Renut's XenosRecomp takes its containers as they are). A normal variable, so it
# shadows gpu.cmake's cache entry. Renut's runtime captures (renut_shader_capture_dir) are added to it by the build.
set(RENUT_SHADER_DUMP_DIR "${CMAKE_SOURCE_DIR}/../out/renut_shader_dump")
if(NOT EXISTS "${RENUT_SHADER_DUMP_DIR}/manifest.csv")
    message(FATAL_ERROR "No ${RENUT_SHADER_DUMP_DIR}/manifest.csv: run tools/renut_dump_shaders.py first")
endif()
set(RENUT_ASSETS_DIR "${CMAKE_SOURCE_DIR}/../assets/game_root" CACHE PATH "Game assets (default.xex)")

# Renut's XenosRecomp is built as an ExternalProject (gpu.cmake): it gets this build's whole host toolchain, not only
# the compiler paths. On the PC, clang for the MSVC ABI.
set(BANJO_RENUT_XENOS_RECOMP_ARGS "")
if(FUTARI_DXC AND NOT DXC_SYSTEM_TOOL)
    set(DXC_SYSTEM_TOOL "${FUTARI_DXC}")  # the DXC of the host shaders (gpu.cmake) for XenosRecomp too
endif()
# DXC_SYSTEM_TOOL / DXC_SYSTEM_LIB: a DirectXShaderCompiler outside the PATH and the library folders, for Linux AArch64
# (thirdparty/reblue-XenosRecomp/thirdparty/dxc-bin/CMakeLists.txt; docs/building-linux.md).
foreach(_futari_var CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET CMAKE_LINKER_TYPE CMAKE_C_FLAGS CMAKE_CXX_FLAGS
                    CMAKE_AR CMAKE_RC_COMPILER CMAKE_MT CMAKE_MAKE_PROGRAM DXC_SYSTEM_TOOL DXC_SYSTEM_LIB)
    if(DEFINED ${_futari_var} AND NOT "${${_futari_var}}" STREQUAL "")
        list(APPEND BANJO_RENUT_XENOS_RECOMP_ARGS "-D${_futari_var}=${${_futari_var}}")
    endif()
endforeach()

# Renut's SDK exports o1heap as rex::o1heap; Futari's SDK names it o1heap.
if(TARGET o1heap AND NOT TARGET rex::o1heap)
    add_library(rex::o1heap ALIAS o1heap)
endif()

# The Switch build is a cross build on a Windows host: Renut's XenosRecomp (a host tool) cannot be built with the
# console's compiler. Its shader cache comes from a PC build of the same tree (tools/renut_switch_shader_cache.py
# writes a copy without the DXIL arrays, which only Windows reads).
if(REXGLUE_PLATFORM_SWITCH)
    # plume's Switch backend: VK_NN_vi_surface on libnx's window and Mesa's static NVK, the same driver the SDK links
    # (REXGLUE_SWITCH_NVK_SDK), and the deko3d back end (renut_gpu_backend = "deko3d").
    set(PLUME_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
    set(PLUME_SWITCH_DEVKITPRO "${DEVKITPRO}" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_ROOT "${REXGLUE_SWITCH_NVK_SDK}" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_LIBRARY "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a" CACHE FILEPATH "" FORCE)
    set(PLUME_SWITCH_LINK_NVK ${FUTARI_SWITCH_NVK} CACHE BOOL "" FORCE)  # futari-nx: app/CMakeLists.txt
    set(PLUME_SWITCH_UAM_DIR "${CMAKE_SOURCE_DIR}/thirdparty/uam" CACHE PATH "" FORCE)
    set(PLUME_DEKO3D ON CACHE BOOL "deko3d back end (Switch)" FORCE)
    # The host shaders' deko3d blobs (gpu.cmake renut_host_shader) need the PC tools tools/deko3d_build_tools.sh builds.
    set(BANJO_DEKO3D_HOST_SHADERS ON)
    set(BANJO_DEKO3D_SPIRV_CROSS "${CMAKE_SOURCE_DIR}/../out/deko3d_tools/spirv-cross-build/spirv-cross.exe" CACHE FILEPATH
        "SPIRV-Cross for the deko3d host shaders (tools/deko3d_build_tools.sh)")
    set(BANJO_DEKO3D_UAM "${CMAKE_SOURCE_DIR}/../out/deko3d_tools/uam-build/uam.exe" CACHE FILEPATH
        "uam (thirdparty/uam built for the PC) for the deko3d host shaders (tools/deko3d_build_tools.sh)")
    foreach(_futari_tool BANJO_DEKO3D_SPIRV_CROSS BANJO_DEKO3D_UAM)
        if(NOT EXISTS "${${_futari_tool}}")
            message(FATAL_ERROR "No ${${_futari_tool}}: run tools/deko3d_build_tools.sh first")
        endif()
    endforeach()
    # futari-nx (2026-10-20, peggle2-nx's design): ON = the files of tools/renut_release_shaders.py
    # (out/renut_switch_release): no translated game shader in the NRO and the shader table without SPIR-V; the game's
    # shaders come from deko3d_glsl.bin beside the NRO, which the web installer makes from the player's executables
    # (for tests: out/renut_switch_release/sd/deko3d_glsl.bin). OFF = every translated shader built in (development,
    # out/renut_switch, tools/renut_deko3d_shaders.sh).
    option(FUTARI_RENUT_RELEASE_SHADERS "Switch: no translated game shader in the NRO (deko3d_glsl.bin beside it)" ON)
    if(FUTARI_RENUT_RELEASE_SHADERS)
        set(_futari_renut_shaders "${CMAKE_SOURCE_DIR}/../out/renut_switch_release")
    else()
        set(_futari_renut_shaders "${CMAKE_SOURCE_DIR}/../out/renut_switch")
    endif()
    # The game shaders' GLSL for deko3d (tools/deko3d_shader_package.py), embedded next to the SPIR-V cache.
    set(BANJO_DEKO3D_GLSL_CPP "${_futari_renut_shaders}/deko3d_glsl_switch.cpp" CACHE FILEPATH
        "The deko3d GLSL package's source (tools/deko3d_shader_package.py)" FORCE)
    set(BANJO_RENUT_SHADER_CACHE_CPP "${_futari_renut_shaders}/shader_cache_switch.cpp" CACHE FILEPATH
        "Renut's shader cache for the Switch build (from a PC build: tools/renut_switch_shader_cache.py)" FORCE)
    function(banjo_renut_shader_cache_override TARGET)
        if(NOT EXISTS "${BANJO_RENUT_SHADER_CACHE_CPP}")
            message(FATAL_ERROR "No ${BANJO_RENUT_SHADER_CACHE_CPP}: build the PC tree first, then run "
                                "tools/renut_switch_shader_cache.py (Renut's XenosRecomp runs on the host)")
        endif()
        target_sources(${TARGET} PRIVATE "${BANJO_RENUT_SHADER_CACHE_CPP}")
        set_source_files_properties("${BANJO_RENUT_SHADER_CACHE_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endfunction()
endif()

include("${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/gpu.cmake")

add_library(futari_renut_gpu OBJECT
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_select.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_tuning.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_checks.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_record.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/display_sync.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/perf/frame_stats.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/futari_renut_shim.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/futari_renut_vblank.cpp")
target_include_directories(futari_renut_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}"
    # banjo_renut_tuning.cpp: the Switch thread helpers (threading_switch.h, through src/me/me_platform_compat.h).
    "${REXSDK_DIR}/src/core")
target_compile_definitions(futari_renut_gpu PRIVATE FUTARI_RENUT_RENDERER=1)
target_link_libraries(futari_renut_gpu PRIVATE rex::runtime)
if(REXGLUE_PLATFORM_SWITCH)
    # The deko3d back end's glue and the game shaders' GLSL package.
    target_sources(futari_renut_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_deko3d.cpp")
    if(EXISTS "${BANJO_DEKO3D_GLSL_CPP}")
        target_sources(futari_renut_gpu PRIVATE "${BANJO_DEKO3D_GLSL_CPP}")
        set_source_files_properties("${BANJO_DEKO3D_GLSL_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    elseif(FUTARI_RENUT_RELEASE_SHADERS)
        message(STATUS "futari: no GLSL package in the NRO (FUTARI_RENUT_RELEASE_SHADERS): deko3d_glsl.bin beside it")
    else()
        message(WARNING "No ${BANJO_DEKO3D_GLSL_CPP}: renut_gpu_backend = deko3d will have no game shaders "
                        "(tools/deko3d_shader_package.py)")
    endif()
endif()
if(COMMAND rexglue_apply_target_settings)
    rexglue_apply_target_settings(futari_renut_gpu)
endif()
renut_gpu_setup(futari_renut_gpu)
# The renderer's own code without the game code's -ffp-contract / aliasing rules: it is host code (as in banjo-nx).
target_link_libraries(futari PRIVATE futari_renut_gpu)
target_compile_definitions(futari PRIVATE FUTARI_RENUT_RENDERER=1)

# The cores' forwarding (generated by tools/renut_futari_hooks.py) and their own hooks' hand-over. The define goes on
# those two sources only: on the whole core target it would change the command line of every generated core file.
foreach(_core palm_normal palm_abnormal reco_normal reco_abnormal hirow_normal hirow_abnormal kiniro_normal kiniro_abnormal)
    if(TARGET futari_${_core})
        target_sources(futari_${_core} PRIVATE src/renut_engine/futari_renut_core_hooks.cpp)
    endif()
endforeach()
set_source_files_properties(src/renut_engine/futari_renut_core_hooks.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_property(SOURCE src/renut_engine/futari_renut_core_hooks.cpp src/native/futari_native_core.cpp
             APPEND PROPERTY COMPILE_DEFINITIONS FUTARI_RENUT_RENDERER=1)
