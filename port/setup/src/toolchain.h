// The C++ tools the game is built with on the player's PC: Microsoft's (Visual Studio or Build Tools: the C++
// library, the Windows SDK) and clang (LLVM's own install, or Visual Studio's "C++ Clang Compiler for Windows").
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "util.h"

namespace toolchain {

struct Toolchain {
    bool ok = false;        // Microsoft's C++ tools are usable
    fs::path vcvars;        // ...\VC\Auxiliary\Build\vcvars64.bat
    std::string version;    // tools version, e.g. "14.51.36231"
    fs::path clang;         // clang++.exe (empty: not installed)
    std::string clang_version;
    std::string problem;    // why it cannot be used (missing, too old, no clang)
    // What Microsoft's installer can do about it (FixTools), and on which install.
    enum class Fix { None, Install, Update, AddCpp, AddClang } fix = Fix::None;
    fs::path fix_path;       // the Visual Studio install to update or modify
    std::string fix_product; // its product id (Microsoft.VisualStudio.Product.BuildTools, ...)
    bool Ready() const { return ok && !clang.empty(); }
};

// `min_msvc_minor`: the 14.NN of the C++ tools the release's prebuilt libraries were made with (an older C++ library
// cannot link them); 0 = any.
Toolchain Find(int min_msvc_minor);
// Makes the tools ready with Microsoft's own installer: installs Build Tools for Visual Studio 2026 (C++ workload and
// clang; the installer is downloaded from Microsoft), updates an install whose tools are too old, or adds what is
// missing. Shows Microsoft's progress window and asks Windows for admin rights; blocks until it ends.
bool FixTools(const Toolchain& t, const std::function<void(const std::string&)>& note, std::string& error);
// The label for the setup's button ("Install for me", ...), empty when there is nothing to do.
const char* FixLabel(const Toolchain& t);

// The environment vcvars64.bat sets up (PATH, INCLUDE, LIB, ...), as a CreateProcess block.
bool CaptureEnvironment(const fs::path& vcvars, std::vector<wchar_t>& block);
std::wstring EnvValue(const std::vector<wchar_t>& block, const wchar_t* name);
// The same environment with `dir` first on PATH.
std::vector<wchar_t> WithPathFirst(const std::vector<wchar_t>& block, const fs::path& dir);
// The same environment with one variable set.
std::vector<wchar_t> WithVar(const std::vector<wchar_t>& block, const std::wstring& name, const std::wstring& value);

} // namespace toolchain
