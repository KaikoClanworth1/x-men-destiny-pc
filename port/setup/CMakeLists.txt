# XMD Setup: the installer and launcher of the X-Men Destiny PC port (XMDSetup.exe). A project of its own, built into
# its own folder (port\installer\setup_build): nothing here is part of the game's build.
#   cmake -S port/setup -B port/installer/setup_build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
#   cmake --build port/installer/setup_build
cmake_minimum_required(VERSION 3.25)
project(XMDSetup LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The C runtime inside the exe: setup must start on a PC that has no Visual C++ runtime installed yet.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Dear ImGui (MIT): the copy in the ReXGlue SDK tree, or any other (-DIMGUI_DIR=...).
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/rexglue-sdk/thirdparty/imgui" CACHE PATH "Dear ImGui source tree")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui not found at ${IMGUI_DIR} (set -DIMGUI_DIR=<its folder>)")
endif()

# The release's version (port\VERSION.txt, one line) and the GitHub repository the launcher looks for updates in.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt" XMD_VERSION LIMIT_COUNT 1)
string(STRIP "${XMD_VERSION}" XMD_VERSION)
set(XMD_REPO "" CACHE STRING "GitHub owner/name of the port's repository (empty: no update check)")

# The exe's icon: the port's own mark, drawn at configure time (tools/make_icon.ps1). The same file goes into the
# release for the game's exe.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/setup.ico")
    execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.ps1" -Out "${CMAKE_BINARY_DIR}/setup.ico" RESULT_VARIABLE ICON_RESULT)
    if(ICON_RESULT)
        message(FATAL_ERROR "tools/make_icon.ps1 failed")
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/setup.rc" "1 ICON \"setup.ico\"
")

add_executable(XMDSetup WIN32
    src/main.cpp
    src/cli.cpp
    src/wizard.cpp
    src/builder.cpp
    src/toolchain.cpp
    src/install_ops.cpp
    src/updater.cpp
    src/xiso.cpp
    src/util.cpp
    ${CMAKE_BINARY_DIR}/setup.rc
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(XMDSetup PRIVATE src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(XMDSetup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    "XMD_VERSION=\"${XMD_VERSION}\"" "XMD_REPO=\"${XMD_REPO}\"")
target_link_libraries(XMDSetup PRIVATE d3d11 dxgi d3dcompiler bcrypt shell32 ole32 uuid shlwapi dwmapi imm32 windowscodecs)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # No build paths in the exe (it is published): __FILE__ and debug info name files relative to the project.
    target_compile_options(XMDSetup PRIVATE "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=" "-ffile-prefix-map=${IMGUI_DIR}/=imgui/")
endif()
