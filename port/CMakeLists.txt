# xmd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(xmd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(XMD_SOURCES
    src/main.cpp
    src/xmd_app.cpp
    src/script_input.cpp
    src/crash_report.cpp
    src/lua_debug.cpp
    src/profile_sampler.cpp
    src/problem_log.cpp
    src/debug_commands.cpp
    src/perf_overlay.cpp
    src/capture.cpp
    src/state_capture.cpp
    src/history.cpp
    src/mod_menu.cpp
    src/pc_menu.cpp
    src/menu_patch.cpp
    src/controls.cpp
)
# The program's icon (res/xmd.ico: the game's own title image).
if(WIN32)
    enable_language(RC)
    list(APPEND XMD_SOURCES res/xmd.rc)
endif()
# Developer build: -DXMD_D3D_TRACE=ON records every call into the game's
# Direct3D library (src/d3d_trace.cpp) instead of the light probe.
option(XMD_D3D_TRACE "Census build: hook every D3D library function" OFF)
# Developer build: -DXMD_D3D_PROBE=ON, the light probe of the D3D functions
# (src/d3d_probe.cpp) instead of the native renderer - both hook them.
option(XMD_D3D_PROBE "Probe build: check the D3D function map" OFF)
if(XMD_D3D_TRACE)
    list(APPEND XMD_SOURCES src/d3d_trace.cpp src/d3d_trace_hooks.cpp)
elseif(XMD_D3D_PROBE)
    list(APPEND XMD_SOURCES src/d3d_probe.cpp)
else()
    # The native renderer: the game's own D3D calls drawn through plume
    # (Direct3D 12, Vulkan, Direct3D 11), with the game's shaders converted
    # ahead of time (tools/convert_shaders.py).
    set(XMD_NATIVE ON)
    list(APPEND XMD_SOURCES
        src/native/gpu.cpp
        src/native/backend_vulkan.cpp
        src/native/recording_list.cpp
        src/native/native_renderer.cpp
        src/native/native_hooks.cpp
        src/native/native_flush_hooks.cpp
        src/native/textures.cpp
        src/native/texture_packs.cpp)
    # The texture layout / tiling code of the SDK's Xenos GPU plugin (not
    # exported by the plugin DLL), so native textures match the emulated ones.
    list(APPEND XMD_SOURCES
        ${REXSDK_DIR}/src/graphics/pipeline/texture/util.cpp
        ${REXSDK_DIR}/src/graphics/pipeline/texture/info_formats.cpp)
    if(WIN32)
        list(APPEND XMD_SOURCES src/native/backend_d3d12.cpp src/native/backend_d3d11.cpp)
    endif()
    # plume (github.com/renderbag/plume, MIT), the copy the SvR 2011 port
    # patched (..\recomp\plume).
    set(PLUME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/plume" CACHE PATH "plume source tree")
    add_subdirectory(${PLUME_DIR} plume EXCLUDE_FROM_ALL)
endif()
if(WIN32)
    list(APPEND XMD_SOURCES src/xaudio2_audio.cpp)
endif()

if(WIN32)
    add_executable(xmd WIN32 ${XMD_SOURCES})
else()
    add_executable(xmd ${XMD_SOURCES})
endif()

rexglue_setup_target(xmd GPU_PLUGINS xenos)
target_include_directories(xmd PRIVATE src)
if(XMD_NATIVE)
    target_link_libraries(xmd PRIVATE plume)
    target_compile_definitions(xmd PRIVATE XMD_NATIVE=1)
endif()

if(XMD_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Names and line numbers of the renderer's own functions in the PDB (src/profile_sampler.cpp, crash reports).
    set_source_files_properties(src/native/native_renderer.cpp src/native/textures.cpp src/native/native_hooks.cpp
        src/native/native_flush_hooks.cpp PROPERTIES COMPILE_OPTIONS "-gline-tables-only")
endif()

if(REXSDK_DIR AND TARGET imgui)
    # SDK source tree: ImGui (built into the runtime) is not on the public
    # include path the way the installed SDK bundles it.
    target_include_directories(xmd PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
        ${REXSDK_DIR}/thirdparty/xxHash
        ${REXSDK_DIR}/thirdparty/stb)
endif()
if(WIN32)
    target_link_libraries(xmd PRIVATE winmm dbghelp xaudio2 ole32 d3d12 d3d11 dxgi dxguid)
    # d3d12.dll only when the game draws with Direct3D 12.
    target_link_options(xmd PRIVATE -Wl,/DELAYLOAD:d3d12.dll)
    target_link_libraries(xmd PRIVATE delayimp)
    # A PDB beside the exe: crash reports name the recompiled function (sub_XXXXXXXX).
    target_link_options(xmd PRIVATE -Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF)
endif()



