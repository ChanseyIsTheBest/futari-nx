#==========================================================
# rexglue_switch.cmake - Nintendo Switch (devkitA64 + libnx)
#
#   rexglue_switch_configure_target(<target> [GPU_PLUGINS ...])
#       what rexglue_configure_target does on Switch
#   rexglue_switch_add_nro(<target> [NAME ..] [AUTHOR ..] [VERSION ..] [ICON ..])
#       packages <target>.nro next to the executable
#
# Requires the switch-devkitA64.cmake toolchain (DEVKITPRO, compiler, specs) and
# REXGLUE_SWITCH_NVK_SDK pointing at the Mesa Horizon SDK.
#==========================================================
include_guard(GLOBAL)

set(REXGLUE_SWITCH_NVK_SDK "" CACHE PATH
    "portlibs/switch directory of the Mesa Horizon SDK (static NVK driver)")
if(NOT EXISTS "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a")
    message(FATAL_ERROR
        "REXGLUE_SWITCH_NVK_SDK must point at <mesa-...-horizon-sdk>/opt/devkitpro/portlibs/switch "
        "(no lib/libvulkan.a under '${REXGLUE_SWITCH_NVK_SDK}')")
endif()

# Static NVK. Not find_package(Vulkan): the Mesa SDK's config requires libelf,
# which devkitPro does not package. switch_elf_stubs.c satisfies the linker
# instead; the NVIDIA CUBIN parser that uses libelf never runs on Switch.
# Listed twice for archive rescanning, as the Mesa config does without
# LINK_GROUP support.
if(NOT TARGET rex::nvk)
    add_library(rexglue_switch_nvk INTERFACE)
    add_library(rex::nvk ALIAS rexglue_switch_nvk)
    target_link_directories(rexglue_switch_nvk INTERFACE
        "${REXGLUE_SWITCH_NVK_SDK}/lib"
        "${DEVKITPRO}/portlibs/switch/lib"
        "${DEVKITPRO}/libnx/lib")
    # The driver by full path, not -lvulkan: the devkitPro toolchain puts -L${DEVKITPRO}/portlibs/switch/lib ahead of
    # these directories, and that folder holds an older generic libvulkan.a (Mesa 26.2.1 git-012c67677c), which every
    # NRO linked until 2026-10-06 instead of the SDK's (nfsmw Mesa 26.2.2; banjo-nx's finding). Check an ELF with
    # `strings -n 8 futari | grep "Mesa 26"`.
    target_link_libraries(rexglue_switch_nvk INTERFACE
        "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a" expat zstd z nx
        "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a" expat zstd z nx
        stdc++ m)
    target_link_options(rexglue_switch_nvk INTERFACE -pthread)
endif()

# Startup objects every title needs, built here, in the SDK's scope, where C is
# enabled: a title project may declare CXX only, and CMake cannot compile a C
# source added to a target of that directory. Linked as an OBJECT library, the
# objects reach the executable directly, never through an archive:
#   runtime_switch.cpp  libnx startup constants (applet type, heap, exception
#                       stack); they override weak libnx symbols
#   switch_elf_stubs.c  libelf stubs for Mesa's NVK
#   switch_crash_hooks.c  abort()/exit() reports and stderr to the SD; abort
#                       must be defined before libc.a is searched
if(NOT TARGET rexglue_switch_startup)
    add_library(rexglue_switch_startup OBJECT
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/runtime_switch.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_elf_stubs.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_crash_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_perf.cpp"
        # FPS and resolution for the console overlays (SaltyNX) and Reverse-NX reading.
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_saltynx.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_sysclk.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_apm.cpp")
    # switch_perf.cpp registers every thread (the profiler needs their handles)
    # by wrapping libnx's threadCreate/threadClose, which pthread goes through.
    # It also counts the libnx calls that cost IPC or walk memory (fences,
    # submissions, NvMap, GPU addresses and mappings, armDCacheClean, the window
    # queue and svcSleepThread) and the time the calling threads spend inside.
    target_link_options(rexglue_switch_startup INTERFACE
        "LINKER:--wrap=threadCreate,--wrap=threadClose"
        "LINKER:--wrap=nvFenceWait,--wrap=nvGpuChannelKickoff,--wrap=nvMapCreate"
        "LINKER:--wrap=nvAddressSpaceAllocFixed,--wrap=nvioctlNvhostAsGpu_MapBufferEx"
        "LINKER:--wrap=armDCacheClean,--wrap=nwindowQueueBuffer,--wrap=bqDequeueBuffer"
        "LINKER:--wrap=svcSleepThread")
endif()

#==========================================================
# rexglue_switch_static_module(<object library> HOST <executable>)
#
# A recompiled guest DLL, linked into the NRO: Horizon has no shared libraries.
# Its objects (hidden visibility, as for a DLL) are partially linked into one
# relocatable object, with the section groups (inline functions, templates)
# resolved inside it (--force-group-allocation), and every hidden symbol is then
# made local. Several modules can therefore define the same sub_XXXXXXXX without
# clashing: Mushihimesama Futari ships eight game-core DLLs built for one base
# address, loaded one at a time. Only ReXModule_Register / ReXModule_GetImageInfo
# stay global, renamed after the module, and a generated source registers them
# (KernelState::RegisterStaticModuleLibrary) under the module's library name, which
# LoadUserModule then uses instead of loading a library.
#==========================================================
function(rexglue_switch_static_module target_name)
    cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})
    if(NOT ARG_HOST)
        message(FATAL_ERROR "rexglue_switch_static_module(${target_name}): HOST is required")
    endif()
    string(MAKE_C_IDENTIFIER "${target_name}" _id)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/rexglue_static_modules")
    set(_partial "${_dir}/${target_name}.partial.o")
    set(_local "${_dir}/${target_name}.o")
    set(_register "${_dir}/${target_name}_register.cpp")
    file(MAKE_DIRECTORY "${_dir}")
    # GCC's precompiled header (cmake_pch.hxx.gch) is listed among the target's objects: not an ELF object.
    set(_objects "$<FILTER:$<TARGET_OBJECTS:${target_name}>,EXCLUDE,[.]gch$>")
    add_custom_command(OUTPUT "${_local}"
        COMMAND "${CMAKE_CXX_COMPILER}" -r -nostdlib -nostartfiles -Wl,--force-group-allocation
                -o "${_partial}" "${_objects}"
        COMMAND "${CMAKE_OBJCOPY}" --localize-hidden
                --redefine-sym "ReXModule_Register=ReXModule_Register_${_id}"
                --redefine-sym "ReXModule_GetImageInfo=ReXModule_GetImageInfo_${_id}"
                "${_partial}" "${_local}"
        DEPENDS "${_objects}"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Linking guest module ${target_name} into one object with local symbols")
    file(WRITE "${_register}.in"
        "// Generated by rexglue_switch_static_module - DO NOT EDIT\n"
        "#include <rex/system/kernel_state.h>\n\n"
        "extern \"C\" void ReXModule_Register_${_id}(rex::runtime::IModuleRegistrar*);\n"
        "extern \"C\" const rex::PPCImageInfo* ReXModule_GetImageInfo_${_id}();\n\n"
        "namespace {\n"
        "const bool g_registered = (rex::system::KernelState::RegisterStaticModuleLibrary(\n"
        "    \"${target_name}\", &ReXModule_Register_${_id}, &ReXModule_GetImageInfo_${_id}), true);\n"
        "}  // namespace\n")
    configure_file("${_register}.in" "${_register}" COPYONLY)
    set_source_files_properties("${_local}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_custom_target(${target_name}_linked DEPENDS "${_local}")
    # The host may be declared after the module (dll_targets.cmake is included first): deferred, with the variables
    # expanded now.
    cmake_language(EVAL CODE "
        cmake_language(DEFER CALL target_sources ${ARG_HOST} PRIVATE \"${_local}\" \"${_register}\")
        cmake_language(DEFER CALL target_link_libraries ${ARG_HOST} PRIVATE rex::runtime)
        cmake_language(DEFER CALL add_dependencies ${ARG_HOST} ${target_name}_linked)")
endfunction()

function(rexglue_switch_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/windowed_app_main_switch.cpp
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)
    target_link_libraries(${target_name} PRIVATE rexglue_switch_startup)

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")
    # The SDK headers the title includes use C++23 (std::byteswap in rex/types.h).
    target_compile_features(${target_name} PRIVATE cxx_std_23)

    # Every object of the runtime and of the GPU plugin goes into the title, as
    # the object files of the SDL build would. From an archive the linker only
    # pulls members that resolve an undefined symbol, and several do nothing
    # but override a weak libnx symbol (__libnx_exception_handler, which the
    # guest memory needs), register themselves from a static constructor
    # (cvars, kernel exports) or serve the plugin entry point.
    set(_whole "$<TARGET_FILE:rexruntime>")
    set(_libs rexruntime)
    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(NOT TARGET rexgpu-${_plugin})
            message(FATAL_ERROR
                "rexglue_switch_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin})")
        endif()
        list(APPEND _whole "$<TARGET_FILE:rexgpu-${_plugin}>")
        list(APPEND _libs rexgpu-${_plugin})
    endforeach()
    string(JOIN "," _whole_csv ${_whole})
    target_link_options(${target_name} PRIVATE
        "LINKER:--whole-archive,${_whole_csv},--no-whole-archive")
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS ${_whole})
    # Still linked normally, for the build order and their own dependencies
    # (glslang, FFmpeg, ...), which stay regular archives.
    target_link_libraries(${target_name} PRIVATE ${_libs} rex::nvk)

    # rex_app.cpp includes imgui.h. rexruntime links imgui privately, so its
    # include path does not reach the title; take the path alone, since imgui's
    # objects are already inside rexruntime.
    target_include_directories(${target_name} PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endfunction()

function(rexglue_switch_add_nro target_name)
    cmake_parse_arguments(ARG "" "NAME;AUTHOR;VERSION;ICON" "" ${ARGN})
    if(NOT ARG_NAME)
        set(ARG_NAME "${target_name}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ReXGlue")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "${PROJECT_VERSION}")
        if(NOT ARG_VERSION)
            set(ARG_VERSION "1.0.0")
        endif()
    endif()
    if(NOT ARG_ICON)
        set(ARG_ICON "${DEVKITPRO}/libnx/default_icon.jpg")
    endif()

    find_program(REXGLUE_NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
    find_program(REXGLUE_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)

    # Changing the icon must re-run packaging even when executable sources are unchanged.
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS "${ARG_ICON}")
    set(_dir "$<TARGET_FILE_DIR:${target_name}>")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${REXGLUE_NACPTOOL}" --create "${ARG_NAME}" "${ARG_AUTHOR}" "${ARG_VERSION}"
            "${_dir}/${target_name}.nacp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${target_name}>"
            "${_dir}/${target_name}.stripped.elf"
        COMMAND "${CMAKE_STRIP}" --strip-all "${_dir}/${target_name}.stripped.elf"
        COMMAND "${REXGLUE_ELF2NRO}" "${_dir}/${target_name}.stripped.elf"
            "${_dir}/${target_name}.nro" "--nacp=${_dir}/${target_name}.nacp" "--icon=${ARG_ICON}"
        COMMENT "Packaging ${target_name}.nro"
        VERBATIM)
endfunction()
