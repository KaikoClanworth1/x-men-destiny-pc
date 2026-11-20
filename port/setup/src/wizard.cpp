// The setup's window (Dear ImGui on Win32 + Direct3D 11): Game files > Build > Play for a new install, and the
// installed game's launcher (Play, Update or repair, Uninstall) when the game is there already.
//
// Test runs (no clicks needed, nothing shown): XMD_SETUP_HIDDEN=1 keeps the window hidden, XMD_SETUP_PAGE=<files|
// build|done|installed|uninstall|uninstalled> opens that page, XMD_SETUP_SHOT=<file.png> saves a picture of the window
// after a few frames and closes it, XMD_SETUP_GAME=<iso or folder> / XMD_SETUP_INSTALL=<folder> / XMD_SETUP_WORK=<folder>
// fill the first page, XMD_SETUP_AUTO=1 presses "Build the game" when nothing blocks it and "Close" when it is done,
// XMD_SETUP_AUTO_UNINSTALL=<keep|all> uninstalls at once. XMD_SETUP_SANDBOX: see util.h.
#include "wizard.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "builder.h"
#include "install_ops.h"
#include "toolchain.h"
#include "updater.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wizard {

namespace {

using util::Format;
using util::Utf8;

// ---------------------------------------------------------------------------------------------------- the window
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
float g_ui = 1.0f; // the display's scale (1 at 100 %)
ImFont* g_font = nullptr;
ImFont* g_bold = nullptr;
std::vector<fs::path> g_dropped;
bool g_resized = false;

float S(float v) { return std::floor(v * g_ui + 0.5f); }

constexpr ImU32 kAccent = IM_COL32(0x3D, 0x6B, 0xF0, 255), kAccentHi = IM_COL32(0x56, 0x80, 0xFF, 255);
constexpr ImU32 kGreen = IM_COL32(0x3F, 0xB2, 0x7F, 255), kRed = IM_COL32(0xE5, 0x53, 0x4B, 255);
constexpr ImU32 kAmber = IM_COL32(0xE3, 0xA3, 0x3B, 255), kDim = IM_COL32(0x9A, 0x9E, 0xAC, 255);
constexpr ImU32 kText = IM_COL32(0xEC, 0xEE, 0xF4, 255), kPanel = IM_COL32(0x33, 0x36, 0x40, 235);
constexpr ImU32 kLine = IM_COL32(0x4A, 0x4E, 0x5C, 255);

void CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_swap && wp != SIZE_MINIMIZED) {
            if (g_rtv)
                g_rtv->Release(), g_rtv = nullptr;
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateTarget();
            g_resized = true;
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = LONG(860 * g_ui);
        mm->ptMinTrackSize.y = LONG(560 * g_ui);
        return 0;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t path[1024];
            if (DragQueryFileW(drop, i, path, 1024))
                g_dropped.emplace_back(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool SavePng(const fs::path& file) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* copy = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &copy))) {
        g_ctx->CopyResource(copy, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(copy, 0, D3D11_MAP_READ, 0, &m))) {
            IWICImagingFactory* wic = nullptr;
            IWICBitmapEncoder* enc = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            IWICStream* stream = nullptr;
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(d.Width, d.Height))) {
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&fmt);
                std::vector<u8> row(size_t(d.Width) * 4);
                ok = true;
                for (UINT y = 0; y < d.Height && ok; y++) {
                    memcpy(row.data(), static_cast<const u8*>(m.pData) + size_t(y) * m.RowPitch, row.size());
                    for (size_t i = 0; i < row.size(); i += 4) { // RGBA to the encoder's BGRA, opaque
                        std::swap(row[i], row[i + 2]);
                        row[i + 3] = 255;
                    }
                    ok = SUCCEEDED(frame->WritePixels(1, UINT(row.size()), UINT(row.size()), row.data()));
                }
                ok = ok && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
            }
            if (frame) frame->Release();
            if (enc) enc->Release();
            if (stream) stream->Release();
            if (wic) wic->Release();
            g_ctx->Unmap(copy, 0);
        }
        copy->Release();
    }
    back->Release();
    return ok;
}

// The Windows file / folder picker. `filters`: {description, "*.a;*.b"}.
std::vector<fs::path> FileDialog(bool folder, const wchar_t* title, std::vector<std::pair<std::wstring, std::wstring>> filters,
                                 const fs::path& start) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dlg->SetOptions(opts);
    dlg->SetTitle(title);
    std::vector<COMDLG_FILTERSPEC> spec;
    for (auto& f : filters)
        spec.push_back({f.first.c_str(), f.second.c_str()});
    if (!spec.empty())
        dlg->SetFileTypes(UINT(spec.size()), spec.data());
    std::error_code ec;
    fs::path dir = start;
    while (!dir.empty() && !fs::is_directory(dir, ec) && dir != dir.parent_path())
        dir = dir.parent_path();
    IShellItem* item = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(fs::absolute(dir).wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
        if (it)
            it->Release();
    }
    dlg->Release();
    return out;
}

// A path as text. Pictures for the README: XMD_SETUP_SHOW_AS=<this PC's folder>=<the folder to show instead>.
std::string Show(const fs::path& p) {
    std::string s = Utf8(p);
    static const std::string map = Utf8(util::Env(L"XMD_SETUP_SHOW_AS"));
    const size_t eq = map.find('=');
    if (eq != std::string::npos && eq > 0 && util::Lower(s).rfind(util::Lower(map.substr(0, eq)), 0) == 0)
        s = map.substr(eq + 1) + s.substr(eq);
    return s;
}

void OpenInExplorer(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ----------------------------------------------------------------------------------------------------- the state
enum class Page { Files, Build, Done, Installed, Uninstalled };

struct App {
    Page page = Page::Files;
    std::string version = XMD_VERSION;
    fs::path sdk_dir;
    builder::Sdk sdk;

    // the player's copies of the game
    std::mutex mutex; // sources, tools (filled by worker threads)
    std::vector<builder::GameSource> sources;
    int selected = -1;
    std::atomic<bool> scanning{false}, scan_cancel{false};
    std::thread scan_thread;
    std::vector<fs::path> to_probe;

    // the C++ tools
    toolchain::Toolchain tools;
    std::atomic<bool> tools_checked{false}, tools_busy{false};
    std::string tools_note, tools_error;
    std::thread tools_thread;

    char install_dir[1024] = "";
    fs::path build_dir;
    int jobs = 0; // 0: automatic
    bool repair = false;

    // the build
    builder::Progress progress;
    std::thread build_thread;
    std::chrono::steady_clock::time_point build_start{};
    std::chrono::steady_clock::time_point step_start[builder::kStepCount]{};
    double step_secs[builder::kStepCount]{};
    int last_step = -1;
    bool details = false, ask_cancel = false;
    double build_minutes = 0;

    // after it
    setup::Installed installed;
    bool have_installed = false;
    bool desktop = true, start_menu = true;
    bool ask_uninstall = false, keep_saves = true;
    std::vector<std::string> left_behind;
    std::string message; // a line under the launcher's buttons
    updater::Updater update;
    // shaders the game saved while it ran, converted before it starts again
    std::thread shader_thread;
    std::atomic<bool> adding_shaders{false}, shaders_added{false};

    bool quit = false;
    bool auto_run = false;
};
App* g_app = nullptr;

void JoinAll(App& a) {
    a.scan_cancel = true;
    a.progress.cancel = true;
    for (std::thread* t : {&a.scan_thread, &a.tools_thread, &a.build_thread, &a.shader_thread})
        if (t->joinable())
            t->join();
}

void AddSource(App& a, const fs::path& p) {
    builder::GameSource g = builder::Probe(p, a.sdk_dir);
    std::lock_guard lk(a.mutex);
    for (size_t i = 0; i < a.sources.size(); i++)
        if (a.sources[i].path == g.path) {
            a.sources[i] = g;
            if (g.verdict == builder::Verdict::Supported)
                a.selected = int(i);
            return;
        }
    a.sources.push_back(g);
    if (g.verdict == builder::Verdict::Supported && (a.selected < 0 || a.sources[size_t(a.selected)].verdict != builder::Verdict::Supported))
        a.selected = int(a.sources.size()) - 1;
}

void StartScan(App& a, std::vector<fs::path> first) {
    a.scanning = true;
    a.scan_thread = std::thread([&a, first] {
        for (const fs::path& p : first)
            AddSource(a, p);
        for (const fs::path& p : builder::FindGameSources(a.scan_cancel)) {
            if (a.scan_cancel)
                break;
            AddSource(a, p);
        }
        a.scanning = false;
    });
}

void CheckTools(App& a) {
    if (a.tools_busy.exchange(true))
        return;
    if (a.tools_thread.joinable())
        a.tools_thread.join();
    a.tools_thread = std::thread([&a] {
        const toolchain::Toolchain t = toolchain::Find(a.sdk.min_msvc_minor);
        {
            std::lock_guard lk(a.mutex);
            a.tools = t;
        }
        a.tools_checked = true;
        a.tools_busy = false;
    });
}

void FixToolsNow(App& a) {
    if (a.tools_busy.exchange(true))
        return;
    if (a.tools_thread.joinable())
        a.tools_thread.join();
    toolchain::Toolchain t;
    {
        std::lock_guard lk(a.mutex);
        t = a.tools;
        a.tools_error.clear();
        a.tools_note = "Microsoft's installer is running...";
    }
    a.tools_thread = std::thread([&a, t] {
        std::string error;
        const bool ok = toolchain::FixTools(t, [&a](const std::string& s) {
            std::lock_guard lk(a.mutex);
            a.tools_note = s;
        }, error);
        const toolchain::Toolchain now = toolchain::Find(a.sdk.min_msvc_minor);
        std::lock_guard lk(a.mutex);
        a.tools = now;
        a.tools_note.clear();
        a.tools_error = ok ? std::string() : error;
        a.tools_busy = false;
    });
}

int Jobs(const App& a) { return a.jobs > 0 ? a.jobs : builder::DefaultJobs(); }

void StartBuild(App& a) {
    builder::Paths p;
    p.sdk_dir = a.sdk_dir;
    p.install_dir = fs::path(util::Wide(a.install_dir));
    p.work_dir = a.build_dir;
    {
        std::lock_guard lk(a.mutex);
        p.source = a.sources[size_t(a.selected)].path;
    }
    std::error_code ec;
    fs::create_directories(p.install_dir / "UserData" / "logs", ec);
    util::SetLogFile(p.install_dir / "UserData" / "logs" / "setup.log");
    if (a.build_thread.joinable())
        a.build_thread.join();
    a.progress.cancel = false;
    a.progress.finished = false;
    a.progress.running = true;
    a.progress.step = 0;
    a.progress.fraction = 0;
    {
        std::lock_guard lk(a.progress.mutex);
        a.progress.error.clear();
        a.progress.log.clear();
    }
    a.last_step = -1;
    for (double& s : a.step_secs)
        s = 0;
    a.build_start = std::chrono::steady_clock::now();
    a.installed.dir = p.install_dir;
    a.installed.build_dir = p.work_dir;
    if (!fs::equivalent(p.source, p.install_dir, ec)) // (a repair builds from the installed files: the record keeps where they came from)
        a.installed.source = p.source;
    a.installed.package = a.sdk_dir.parent_path();
    a.installed.version = a.sdk.version;
    a.installed.by_launcher = false;
    a.page = Page::Build;
    const int jobs = Jobs(a);
    a.build_thread = std::thread([&a, p, jobs] { builder::BuildGame(p, jobs, a.progress); });
}

// --------------------------------------------------------------------------------------------------------- drawing
void TextColored(ImU32 col, const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void Heading(const char* s) {
    ImGui::PushFont(g_bold, S(13));
    TextColored(kDim, s);
    ImGui::PopFont();
}

bool Button(const char* label, bool primary = false, bool enabled = true, ImVec2 size = ImVec2(0, 0), ImU32 colour = 0) {
    ImGui::BeginDisabled(!enabled);
    const ImU32 base = colour ? colour : primary ? kAccent : IM_COL32(0x45, 0x49, 0x57, 255);
    const ImU32 hi = colour ? colour : primary ? kAccentHi : IM_COL32(0x55, 0x5A, 0x6B, 255);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hi);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, base);
    if (primary)
        ImGui::PushFont(g_bold, S(15));
    const bool hit = ImGui::Button(label, size);
    if (primary)
        ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    return hit;
}

bool BeginPanel(const char* id, float height = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(9));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
    return ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding | (height == 0 ? ImGuiChildFlags_AutoResizeY : 0));
}
void EndPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
}

void Badge(const char* text, ImU32 col) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 pad(S(8), S(3));
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x + pad.x * 2, p.y + size.y + pad.y * 2), (col & 0x00FFFFFF) | 0x38000000, S(10));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad.x, p.y + pad.y), col, text);
    ImGui::Dummy(ImVec2(size.x + pad.x * 2, size.y + pad.y * 2));
}

std::string Minutes(double secs) {
    if (secs < 90)
        return "about a minute";
    return Format("about %d min", int(secs / 60 + 0.5));
}
std::string Clock(double secs) { return Format("%d:%02d", int(secs) / 60, int(secs) % 60); }

// What stops "Build the game", in the order the player should fix it (empty: ready).
std::string Blocking(App& a) {
    if (!a.sdk.problem.empty())
        return a.sdk.problem;
    std::lock_guard lk(a.mutex);
    if (a.selected < 0 || a.sources[size_t(a.selected)].verdict != builder::Verdict::Supported)
        return a.repair ? "The installed game's files are missing: add your disc image again." : "Add your copy of the game.";
    if (!a.tools_checked)
        return "Checking the C++ tools...";
    if (a.tools_busy)
        return "Microsoft's installer is running...";
    if (!a.tools.Ready())
        return "Install the Microsoft C++ Build Tools.";
    if (!a.install_dir[0])
        return "Choose a folder to install to.";
    return {};
}

u64 SpaceNeeded(App& a, const fs::path& install) {
    u64 need = builder::kProgramBytes;
    {
        std::lock_guard lk(a.mutex);
        if (a.selected >= 0)
            need += a.sources[size_t(a.selected)].bytes;
    }
    std::error_code ec;
    const u64 have = fs::exists(install, ec) ? util::FolderBytes(install) : 0;
    need = need > have ? need - have : 0;
    if (util::DriveSpace(install).drive == util::DriveSpace(a.build_dir).drive)
        need += builder::kBuildFilesBytes;
    return need;
}

void PageFiles(App& a, float bottom_h) {
    ImGui::PushFont(g_bold, S(26));
    ImGui::TextUnformatted(a.repair ? "Update or repair your game" : "Build your game");
    ImGui::PopFont();
    TextColored(kDim, a.repair ? "Setup builds the game again from the files already installed. Your saves and settings stay."
                               : "Pick your copy of the game and where the game goes. Setup builds a native PC version from it, once.");
    ImGui::Dummy(ImVec2(0, S(8)));

    if (BeginPanel("game")) {
        Heading("YOUR GAME");
        std::vector<builder::GameSource> list;
        int selected;
        {
            std::lock_guard lk(a.mutex);
            list = a.sources;
            selected = a.selected;
        }
        if (list.empty())
            TextColored(kDim, a.scanning ? "Looking for your game on this PC..."
                                         : "No copy of the game found yet. Browse for it or drop it on this window.");
        for (size_t i = 0; i < list.size(); i++) {
            const builder::GameSource& g = list[i];
            ImGui::PushID(int(i));
            const bool ok = g.verdict == builder::Verdict::Supported;
            ImGui::BeginDisabled(!ok);
            if (ImGui::RadioButton("##pick", selected == int(i))) {
                std::lock_guard lk(a.mutex);
                a.selected = int(i);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(Utf8(g.path.filename()).c_str());
            TextColored(kDim, g.Describe() + "  \xC2\xB7  " + Show(g.path.parent_path()));
            ImGui::EndGroup();
            const char* badge = g.Badge();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(badge).x - S(18));
            Badge(badge, ok ? kGreen : g.verdict == builder::Verdict::WrongVersion ? kAmber : kRed);
            if (!g.detail.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", g.detail.c_str());
            ImGui::PopID();
        }
        if (a.scanning && !list.empty())
            TextColored(kDim, "Still looking...");
        ImGui::Dummy(ImVec2(0, S(2)));
        TextColored(kDim, "Not listed?");
        ImGui::SameLine();
        if (Button("Browse...")) {
            for (auto& p : FileDialog(false, L"Your X-Men: Destiny disc image", {{L"Xbox 360 disc image (*.iso)", L"*.iso"}, {L"All files", L"*.*"}}, util::SetupDir()))
                AddSource(a, p);
        }
        ImGui::SameLine();
        if (Button("A folder...")) {
            for (auto& p : FileDialog(true, L"The folder your disc was extracted to (it holds Default.xex)", {}, util::SetupDir()))
                AddSource(a, p);
        }
        ImGui::SameLine();
        TextColored(kDim, "or drop it on this window");
    }
    EndPanel();

    if (BeginPanel("tools")) {
        Heading("C++ BUILD TOOLS");
        toolchain::Toolchain t;
        std::string note, error;
        {
            std::lock_guard lk(a.mutex);
            t = a.tools;
            note = a.tools_note;
            error = a.tools_error;
        }
        if (!a.tools_checked) {
            TextColored(kDim, "Checking...");
        } else if (a.tools_busy && !note.empty()) {
            TextColored(kAmber, note);
            TextColored(kDim, "Follow its window. Windows asks for admin rights.");
        } else if (t.Ready()) {
            TextColored(kGreen, "Ready");
            ImGui::SameLine();
            TextColored(kDim, "Microsoft C++ " + t.version + "  \xC2\xB7  clang " + t.clang_version);
        } else {
            TextColored(kAmber, t.problem.empty() ? "Microsoft C++ Build Tools needed" : t.problem);
            TextColored(kDim, "The game is compiled on your PC with Microsoft's free Build Tools (C++ and its clang compiler).");
            if (!error.empty())
                TextColored(kRed, error);
            if (*toolchain::FixLabel(t)) {
                if (Button(toolchain::FixLabel(t), true))
                    FixToolsNow(a);
                ImGui::SameLine();
            }
            if (Button("Get"))
                ShellExecuteW(nullptr, L"open", L"https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio", nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (Button("Check again"))
                CheckTools(a);
        }
    }
    EndPanel();

    if (BeginPanel("where")) {
        Heading("INSTALL THE GAME TO");
        ImGui::BeginDisabled(a.repair);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(96));
        ImGui::InputText("##dir", a.install_dir, sizeof(a.install_dir));
        ImGui::SameLine();
        if (Button("Change", false, true, ImVec2(S(84), 0))) {
            for (auto& p : FileDialog(true, L"Where to install X-Men Destiny", {}, fs::path(util::Wide(a.install_dir)))) {
                const fs::path dir = _wcsicmp(p.filename().c_str(), setup::kGameName) == 0 ? p : p / setup::kGameName;
                strncpy_s(a.install_dir, Utf8(dir).c_str(), _TRUNCATE);
            }
        }
        ImGui::EndDisabled();
        const std::string warn = setup::FolderWarning(fs::path(util::Wide(a.install_dir)));
        if (!warn.empty())
            TextColored(kAmber, warn + " Choose another folder.");
        ImGui::Dummy(ImVec2(0, S(2)));
        TextColored(kDim, "Build files: about " + util::Gb(builder::kBuildFilesBytes) + " in " + Show(a.build_dir) +
                              " (kept, so updates only compile what changed). Builds run at low priority: your PC stays usable.");
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::AlignTextToFramePadding();
        TextColored(kDim, "Build workers");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(150));
        const std::string current = a.jobs > 0 ? std::to_string(a.jobs) : Format("Automatic (%d)", builder::DefaultJobs());
        if (ImGui::BeginCombo("##jobs", current.c_str())) {
            if (ImGui::Selectable(Format("Automatic (%d)", builder::DefaultJobs()).c_str(), a.jobs == 0))
                a.jobs = 0;
            for (int j = 1; j <= int(std::thread::hardware_concurrency()); j++)
                if (ImGui::Selectable(std::to_string(j).c_str(), a.jobs == j))
                    a.jobs = j;
            ImGui::EndCombo();
        }
    }
    EndPanel();
    (void)bottom_h;
}

void BottomFiles(App& a) {
    const fs::path install = fs::path(util::Wide(a.install_dir));
    static u64 need = 0;
    static util::Space space;
    static double next = 0;
    if (ImGui::GetTime() >= next) { // (folder sizes: not every frame)
        need = SpaceNeeded(a, install);
        space = util::DriveSpace(install);
        next = ImGui::GetTime() + 1.0;
    }
    const bool fits = space.free >= need;
    const std::string blocking = Blocking(a);
    const std::string reason = !blocking.empty() ? blocking : !fits ? "Not enough free space: pick another drive." : std::string();
    ImGui::BeginGroup();
    TextColored(fits ? kText : kRed, "Needs " + util::Gb(need) + "  \xC2\xB7  " + space.drive + " has " + util::Gb(space.free) + " free");
    if (!reason.empty())
        TextColored(kAmber, reason);
    else
        TextColored(kDim, "About 20 to 40 minutes on most PCs");
    ImGui::EndGroup();
    const char* label = a.repair ? "Update the game" : "Build the game";
    const ImVec2 size(S(190), S(42));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - size.x);
    if (Button(label, true, reason.empty(), size) || (a.auto_run && reason.empty() && a.tools_checked && !a.scanning))
        StartBuild(a);
}

void Ring(float fraction, float radius, const std::string& label) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + radius, p.y + radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(c, radius - S(5), 0, 6.2832f, 64);
    dl->PathStroke(kLine, 0, S(8));
    if (fraction > 0.001f) {
        dl->PathArcTo(c, radius - S(5), -1.5708f, -1.5708f + 6.2832f * std::clamp(fraction, 0.0f, 1.0f), 64);
        dl->PathStroke(kAccentHi, 0, S(8));
    }
    ImGui::PushFont(g_bold, S(28));
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), kText, label.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

void PageBuild(App& a) {
    builder::Progress& pr = a.progress;
    const auto now = std::chrono::steady_clock::now();
    const int step = pr.step;
    if (step != a.last_step) { // the time each step took
        if (a.last_step >= 0)
            a.step_secs[a.last_step] = std::chrono::duration<double>(now - a.step_start[a.last_step]).count();
        a.step_start[step] = now;
        a.last_step = step;
    }
    const double elapsed = std::chrono::duration<double>(now - a.build_start).count();
    const bool failed = pr.finished && !pr.ok;
    const float overall = pr.Overall();

    ImGui::PushFont(g_bold, S(26));
    ImGui::TextUnformatted(failed ? (pr.cancel ? "Build stopped" : "The build stopped") : "Building your game");
    ImGui::PopFont();
    if (failed)
        TextColored(kDim, "See below for what went wrong. Finished parts are kept: building again carries on.");
    else if (overall > 0.05f && elapsed > 20)
        TextColored(kDim, Minutes(elapsed / overall - elapsed) + " left  \xC2\xB7  your PC stays usable");
    else
        TextColored(kDim, "Usually 20 to 40 minutes  \xC2\xB7  your PC stays usable");
    ImGui::Dummy(ImVec2(0, S(10)));

    Ring(overall, S(74), Format("%d%%", int(overall * 100)));
    ImGui::SameLine(0, S(28));
    ImGui::BeginGroup();
    for (int i = 0; i < builder::kStepCount; i++) {
        const bool done = i < step || (pr.finished && pr.ok), active = i == step && !pr.finished;
        const ImU32 col = done ? kGreen : active ? kText : failed && i == step ? kRed : kDim;
        std::string line = std::string(done ? "\xE2\x9C\x93  " : active ? "\xE2\x80\xA2  " : "    ") + builder::kStepNames[i];
        if (active || (failed && i == step)) {
            if (i == builder::kRead && pr.bytes_total)
                line += "  \xC2\xB7  " + util::Gb(pr.bytes_done) + " of " + util::Gb(pr.bytes_total);
            if (i == builder::kShaders && pr.shaders_total)
                line += Format("  \xC2\xB7  %d / %d", pr.shaders_done.load(), pr.shaders_total.load());
            if (i == builder::kCompile && pr.total)
                line += Format("  \xC2\xB7  %d / %d", pr.done.load(), pr.total.load());
        }
        TextColored(col, line);
        const double secs = done ? a.step_secs[i] : active ? std::chrono::duration<double>(now - a.step_start[i]).count() : 0;
        if (secs >= 1) {
            ImGui::SameLine(S(420));
            TextColored(kDim, Clock(secs));
        }
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(10)));

    if (failed) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x5A, 0x26, 0x24, 235));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(9));
        ImGui::BeginChild("error", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(pr.GetError().c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(6)));
    }
    if (ImGui::SmallButton(a.details ? "Hide details" : "Show details"))
        a.details = !a.details;
    if (a.details) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x18, 0x19, 0x1E, 255));
        ImGui::BeginChild("log", ImVec2(0, S(150)), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(g_font, S(13));
        for (const std::string& l : pr.Tail(300))
            ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(20))
            ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (pr.finished && pr.ok) {
        if (a.build_thread.joinable())
            a.build_thread.join();
        a.build_minutes = elapsed / 60.0;
        a.have_installed = true;
        a.page = Page::Done;
    }
}

void BottomBuild(App& a) {
    builder::Progress& pr = a.progress;
    const ImVec2 size(S(150), S(40));
    if (pr.finished && !pr.ok) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - size.x * 2 - S(10));
        if (Button("Back", false, true, size)) {
            if (a.build_thread.joinable())
                a.build_thread.join();
            a.page = Page::Files;
        }
        ImGui::SameLine();
        if (Button("Try again", true, true, size))
            StartBuild(a);
        if (a.auto_run)
            a.quit = true;
        return;
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - size.x);
    if (Button("Cancel", false, !pr.cancel, size))
        a.ask_cancel = true;
    if (a.ask_cancel) {
        ImGui::OpenPopup("Stop the build?");
        a.ask_cancel = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Stop the build?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Finished parts are kept: building again carries on where it stopped.");
        ImGui::Dummy(ImVec2(0, S(8)));
        if (Button("Keep building", true, true, ImVec2(S(150), S(36))))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (Button("Stop", false, true, ImVec2(S(110), S(36)))) {
            pr.cancel = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Finish(App& a) {
    a.installed.desktop = a.desktop;
    a.installed.start_menu = a.start_menu;
    std::string error;
    if (!setup::FinishInstall(a.installed, error))
        a.message = error;
}

void Play(App& a) {
    const fs::path exe = a.installed.dir / "xmd.exe";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = util::Quote(exe);
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, a.installed.dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        a.quit = true;
    } else {
        a.message = Format("The game could not be started (error %lu).", GetLastError());
    }
}

// Play: first the shaders the game saved the last time it ran (a few seconds), then the game.
void RequestPlay(App& a) {
    if (a.adding_shaders)
        return;
    if (builder::SavedShaderCount(a.installed.dir) == 0 || !a.sdk.problem.empty()) {
        Play(a);
        return;
    }
    if (a.shader_thread.joinable())
        a.shader_thread.join();
    a.adding_shaders = true;
    a.shaders_added = false;
    a.progress.cancel = false;
    builder::Paths p;
    p.sdk_dir = a.sdk_dir;
    p.install_dir = a.installed.dir;
    p.work_dir = a.installed.build_dir.empty() ? a.build_dir : a.installed.build_dir;
    const int jobs = Jobs(a);
    a.shader_thread = std::thread([&a, p, jobs] {
        std::string why;
        builder::AddSavedShaders(p, jobs, a.progress, why); // (a failure is logged; the game starts all the same)
        a.shaders_added = true;
    });
}

void PageDone(App& a) {
    ImGui::PushFont(g_bold, S(26));
    ImGui::TextUnformatted("Ready to play");
    ImGui::PopFont();
    TextColored(kDim, Format("Built in %d minutes  \xC2\xB7  installed to ", std::max(1, int(a.build_minutes + 0.5))) + Show(a.installed.dir));
    ImGui::Dummy(ImVec2(0, S(10)));
    if (BeginPanel("shortcuts")) {
        Heading("SHORTCUTS");
        ImGui::Checkbox("Create a desktop shortcut", &a.desktop);
        ImGui::Checkbox("Add to the Start menu", &a.start_menu);
    }
    EndPanel();
    if (BeginPanel("settings")) {
        Heading("SETTINGS ARE IN THE GAME");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("Resolution, frame rate and the other PC options are in the game's own menu: Options > Graphics.");
        TextColored(kDim, "Your saves and settings live in the game folder (UserData). Updating, repairing and uninstalling leave them alone. "
                          "Run the launcher again, or use Windows Settings > Apps, to update, repair or uninstall.");
        ImGui::PopTextWrapPos();
    }
    EndPanel();
    if (const std::string w = a.progress.GetWarning(); !w.empty())
        TextColored(kAmber, w);
    if (!a.message.empty())
        TextColored(kRed, a.message);
}

void BottomDone(App& a) {
    const ImVec2 size(S(150), S(42));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - size.x * 2 - S(10));
    if (Button("Close", false, true, size) || a.auto_run) {
        Finish(a);
        a.quit = true;
    }
    ImGui::SameLine();
    if (Button("Play", true, true, size)) {
        Finish(a);
        if (a.message.empty())
            RequestPlay(a);
    }
}

void BigButton(const char* title, const char* text, bool primary, const std::function<void()>& click, bool enabled = true) {
    ImGui::PushID(title);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(w, S(62));
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton("card", size);
    const bool hover = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = primary ? (hover ? kAccentHi : kAccent) : hover ? IM_COL32(0x40, 0x44, 0x52, 245) : kPanel;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, S(9));
    ImGui::PushFont(g_bold, S(17));
    dl->AddText(ImVec2(p.x + S(18), p.y + S(10)), enabled ? kText : kDim, title);
    ImGui::PopFont();
    dl->AddText(ImVec2(p.x + S(18), p.y + S(34)), primary ? IM_COL32(0xDD, 0xE5, 0xFF, 255) : kDim, text);
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PopID();
    if (hit)
        click();
}

void StartRepair(App& a) {
    std::error_code ec;
    if (a.sdk_dir.empty() || !a.sdk.problem.empty()) {
        a.message = "The setup package this game was built with is gone. Download the release again and run its XMDSetup.exe.";
        return;
    }
    a.repair = true;
    strncpy_s(a.install_dir, Utf8(a.installed.dir).c_str(), _TRUNCATE);
    if (!a.installed.build_dir.empty())
        a.build_dir = a.installed.build_dir;
    a.desktop = a.installed.desktop;
    a.start_menu = a.installed.start_menu;
    std::vector<fs::path> first;
    if (fs::exists(a.installed.dir / "Default.xex", ec))
        first.push_back(a.installed.dir); // the files already installed
    else if (!a.installed.source.empty() && fs::exists(a.installed.source, ec))
        first.push_back(a.installed.source);
    {
        std::lock_guard lk(a.mutex);
        a.sources.clear();
        a.selected = -1;
    }
    for (const fs::path& p : first)
        AddSource(a, p);
    CheckTools(a);
    a.message.clear();
    a.page = Page::Files;
}

void PageInstalled(App& a) {
    ImGui::PushFont(g_bold, S(26));
    ImGui::TextUnformatted("X-Men Destiny is installed");
    ImGui::PopFont();
    TextColored(kDim, Show(a.installed.dir) + (a.installed.version.empty() ? "" : "  \xC2\xB7  version " + a.installed.version));
    ImGui::Dummy(ImVec2(0, S(8)));

    // a newer release
    using State = updater::Updater::State;
    const State us = a.update.state;
    if (us == State::Available || us == State::Downloading || us == State::Unpacking || us == State::Ready ||
        (us == State::Failed && !a.update.release.version.empty())) {
        std::string title, text;
        std::string error;
        {
            std::lock_guard lk(a.update.mutex);
            title = "New version " + a.update.release.version;
            error = a.update.error;
        }
        if (us == State::Available)
            text = a.installed.by_launcher ? "Update it in your launcher." : "Update now: downloads it and builds the game again (your saves stay)";
        else if (us == State::Downloading)
            text = Format("Downloading %d%%", int(a.update.progress * 100));
        else if (us == State::Unpacking)
            text = "Unpacking";
        else if (us == State::Ready)
            text = "Starting the new setup";
        else
            title = "Update failed", text = "Try again: " + error;
        BigButton(title.c_str(), text.c_str(), false, [&] {
            if ((us == State::Available || us == State::Failed) && !a.installed.by_launcher) {
                a.update.state = State::Available;
                a.update.Start(a.installed.package);
            }
        }, us == State::Available || us == State::Failed);
        if (us == State::Ready && a.update.Run(a.installed.dir))
            a.quit = true;
    }

    std::error_code ec;
    const bool playable = fs::exists(a.installed.dir / "xmd.exe", ec);
    if (a.shaders_added.exchange(false)) {
        if (a.shader_thread.joinable())
            a.shader_thread.join();
        a.adding_shaders = false;
        Play(a);
    }
    BigButton("Play", a.adding_shaders ? "Preparing the shaders the game made last time..."
                      : playable ? "Start the game" : "The game is not built yet: use Update or repair",
              true, [&] { RequestPlay(a); }, playable && !a.adding_shaders);
    BigButton("Update or repair", "Build the game again from the files already installed. Saves and settings stay.", false, [&] { StartRepair(a); });
    BigButton("Saves and settings", "Open the folder with your saves, settings, captures and logs", false, [&] {
        fs::create_directories(a.installed.dir / "UserData", ec);
        OpenInExplorer(a.installed.dir / "UserData");
    });
    BigButton("Uninstall", "Remove the game from this PC", false, [&] { a.ask_uninstall = true; });
    if (!a.message.empty()) {
        ImGui::PushTextWrapPos(0);
        TextColored(kAmber, a.message);
        ImGui::PopTextWrapPos();
    }

    if (a.ask_uninstall) {
        ImGui::OpenPopup("Uninstall X-Men Destiny?");
        a.ask_uninstall = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Uninstall X-Men Destiny?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("The game, its build files and its shortcuts are removed from this PC.");
        TextColored(kDim, "Your own disc image or folder is not touched.");
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::Checkbox("Keep my saves and settings", &a.keep_saves);
        ImGui::Dummy(ImVec2(0, S(8)));
        if (Button("Cancel", false, true, ImVec2(S(120), S(36))))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (Button("Uninstall", false, true, ImVec2(S(120), S(36)), kRed)) {
            a.left_behind = setup::Uninstall(a.installed, a.keep_saves);
            a.page = Page::Uninstalled;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void PageUninstalled(App& a) {
    ImGui::PushFont(g_bold, S(26));
    ImGui::TextUnformatted("Uninstalled");
    ImGui::PopFont();
    TextColored(kDim, a.keep_saves ? "X-Men Destiny is removed. Your saves and settings are kept in " + Utf8(a.installed.dir / "UserData") + "."
                                   : "X-Men Destiny and its saves are removed.");
    if (!a.left_behind.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        TextColored(kAmber, "These could not be removed (in use?). Delete them by hand:");
        for (const std::string& l : a.left_behind)
            ImGui::BulletText("%s", l.c_str());
    }
}

void Frame(App& a) {
    for (const fs::path& p : g_dropped)
        if (a.page == Page::Files)
            AddSource(a, p);
    g_dropped.clear();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0x2C, 0x2E, 0x35, 255),
                                IM_COL32(0x2C, 0x2E, 0x35, 255), IM_COL32(0x1D, 0x1E, 0x23, 255), IM_COL32(0x1D, 0x1E, 0x23, 255));
    bg->AddCircleFilled(ImVec2(vp->Pos.x + vp->Size.x * 0.85f, vp->Pos.y - S(60)), S(300), IM_COL32(0x3D, 0x6B, 0xF0, 18), 64);

    const float top_h = S(52), bottom_h = (a.page == Page::Installed || a.page == Page::Uninstalled) ? S(40) : S(74);
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    // top bar: the name and the steps
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = vp->Pos;
        dl->AddRectFilled(p, ImVec2(p.x + vp->Size.x, p.y + top_h), IM_COL32(0x20, 0x22, 0x28, 200));
        ImGui::PushFont(g_bold, S(16));
        dl->AddText(ImVec2(p.x + S(22), p.y + S(16)), kText, "X-Men Destiny Setup");
        ImGui::PopFont();
        if (a.page == Page::Files || a.page == Page::Build || a.page == Page::Done) {
            const char* names[] = {"Game files", "Build", "Play"};
            const int at = a.page == Page::Files ? 0 : a.page == Page::Build ? 1 : 2;
            float x = p.x + vp->Size.x - S(22);
            for (int i = 2; i >= 0; i--) {
                const ImVec2 ts = ImGui::CalcTextSize(names[i]);
                x -= ts.x + S(24);
                const ImU32 col = i == at ? kAccent : i < at ? IM_COL32(0x3F, 0xB2, 0x7F, 70) : IM_COL32(0x45, 0x49, 0x57, 255);
                dl->AddRectFilled(ImVec2(x, p.y + S(13)), ImVec2(x + ts.x + S(24), p.y + S(39)), col, S(13));
                dl->AddText(ImVec2(x + S(12), p.y + S(26) - ts.y / 2), i == at ? kText : IM_COL32(0xC8, 0xCC, 0xD8, 255), names[i]);
                x -= S(8);
            }
        }
    }

    ImGui::SetCursorPos(ImVec2(0, top_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(18)));
    ImGui::BeginChild("content", ImVec2(vp->Size.x, vp->Size.y - top_h - bottom_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (a.page) {
    case Page::Files: PageFiles(a, bottom_h); break;
    case Page::Build: PageBuild(a); break;
    case Page::Done: PageDone(a); break;
    case Page::Installed: PageInstalled(a); break;
    case Page::Uninstalled: PageUninstalled(a); break;
    }
    const bool more = ImGui::GetScrollMaxY() > 0 && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - S(4);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (more) {
        const char* hint = "More below";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        ImGui::GetWindowDrawList()->AddText(ImVec2(vp->Pos.x + (vp->Size.x - ts.x) / 2, vp->Pos.y + vp->Size.y - bottom_h - ts.y - S(4)), kDim, hint);
    }

    // bottom bar
    ImGui::SetCursorPos(ImVec2(0, vp->Size.y - bottom_h));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x20, 0x22, 0x28, 220));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), a.page == Page::Installed || a.page == Page::Uninstalled ? S(10) : S(14)));
    ImGui::BeginChild("bottom", ImVec2(vp->Size.x, bottom_h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    switch (a.page) {
    case Page::Files: BottomFiles(a); break;
    case Page::Build:
        TextColored(kDim, "Version " + a.version);
        BottomBuild(a);
        break;
    case Page::Done:
        TextColored(kDim, std::string(setup::kCredit) + "  \xC2\xB7  version " + a.version);
        BottomDone(a);
        break;
    case Page::Installed:
    case Page::Uninstalled:
        TextColored(kDim, std::string(setup::kCredit) + "  \xC2\xB7  launcher version " + a.version);
        if (a.page == Page::Uninstalled) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - S(100));
            if (ImGui::SmallButton("  Close  "))
                a.quit = true;
        }
        break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
}

void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 10;
    st.FrameRounding = 7;
    st.PopupRounding = 9;
    st.GrabRounding = 6;
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(9, 7);
    st.WindowPadding = ImVec2(18, 16);
    st.ScrollbarSize = 12;
    st.ScaleAllSizes(g_ui);
    st.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kText);
    st.Colors[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x24, 0x26, 0x2E, 255));
    st.Colors[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x2C, 0x2F, 0x39, 255));
    st.Colors[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x2C, 0x2F, 0x39, 255));
    st.Colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x2A, 0x2C, 0x34, 250));
    st.Colors[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(kAccentHi);
    st.Colors[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x3D, 0x6B, 0xF0, 90));
    st.Colors[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x3D, 0x6B, 0xF0, 140));
    st.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    st.Colors[ImGuiCol_TitleBgActive] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x2A, 0x2C, 0x34, 255));
    st.Colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(kLine);
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    const fs::path fonts = fs::path(win) / "Fonts";
    std::error_code ec;
    auto load = [&](const char* file) -> ImFont* {
        const fs::path p = fonts / file;
        return fs::exists(p, ec) ? io.Fonts->AddFontFromFileTTF(Utf8(p).c_str(), 15.0f) : nullptr;
    };
    g_font = load("segoeui.ttf");
    if (!g_font)
        g_font = io.Fonts->AddFontDefault();
    g_bold = load("seguisb.ttf");
    if (!g_bold)
        g_bold = g_font;
    io.FontDefault = g_font;
    ImGui::GetStyle().FontSizeBase = S(15);
}

} // namespace

int Run(const cli::Args& args) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    App app;
    g_app = &app;
    App& a = app;

    // ---- what is this run: a new install, or the launcher of an installed game?
    const fs::path here = util::SetupDir();
    std::error_code ec;
    const bool as_launcher = _wcsicmp(util::ExePath().filename().c_str(), setup::kLauncherExe) == 0;
    std::optional<setup::Installed> in;
    if (const std::string dir = args.Get("update"); !dir.empty())
        in = setup::LoadInstalled(fs::path(util::Wide(dir)));
    if (!in && (as_launcher || fs::exists(here / "setup.ini", ec)))
        in = setup::LoadInstalled(here);
    if (!in && !util::Env(L"XMD_SETUP_INSTALL").empty())
        in = setup::LoadInstalled(util::Env(L"XMD_SETUP_INSTALL"));
    if (!in && util::Env(L"XMD_SETUP_GAME").empty())
        in = setup::FindInstalled();
    a.sdk_dir = setup::SdkDir();
    if (!fs::exists(a.sdk_dir / "sdk.ini", ec) && in && !in->package.empty())
        a.sdk_dir = in->package / "sdk"; // the launcher: the package the game was built with
    a.sdk = builder::LoadSdk(a.sdk_dir);
    if (!a.sdk.version.empty() && !as_launcher)
        a.version = a.sdk.version;
    a.build_dir = util::Env(L"XMD_SETUP_WORK").empty() ? setup::DefaultBuildDir() : fs::path(util::Env(L"XMD_SETUP_WORK"));
    strncpy_s(a.install_dir, Utf8(util::Env(L"XMD_SETUP_INSTALL").empty() ? setup::DefaultInstallDir() : fs::path(util::Env(L"XMD_SETUP_INSTALL"))).c_str(), _TRUNCATE);
    a.auto_run = !util::Env(L"XMD_SETUP_AUTO").empty();
    util::SetLogFile(in ? in->dir / "UserData" / "logs" / "setup.log" : util::LocalDataDir() / "setup.log");
    util::Log("---- XMD Setup " + a.version + (as_launcher ? " (launcher)" : "") + ": " + Utf8(std::wstring(GetCommandLineW())));

    if (in) {
        a.installed = *in;
        a.have_installed = true;
        a.page = Page::Installed;
        if (args.Has("uninstall"))
            a.ask_uninstall = true;
        if (args.Has("update"))
            StartRepair(a), a.auto_run = true; // the updater's new setup: build at once
        else if (!a.installed.by_launcher)
            a.update.Check(a.version);
    } else {
        std::vector<fs::path> first;
        if (const std::wstring g = util::Env(L"XMD_SETUP_GAME"); !g.empty())
            first.push_back(g);
        if (util::Env(L"XMD_SETUP_NO_SCAN").empty())
            StartScan(a, first);
        else
            for (const fs::path& p : first)
                AddSource(a, p);
        CheckTools(a);
    }
    const std::wstring page = util::Env(L"XMD_SETUP_PAGE");
    if (page == L"files") a.page = Page::Files;
    else if (page == L"build") a.page = Page::Build, a.build_start = std::chrono::steady_clock::now();
    else if (page == L"done") a.page = Page::Done;
    else if (page == L"installed") a.page = Page::Installed;
    else if (page == L"uninstall") a.page = Page::Installed, a.ask_uninstall = true;
    else if (page == L"uninstalled") a.page = Page::Uninstalled;
    if (const std::wstring how = util::Env(L"XMD_SETUP_AUTO_UNINSTALL"); !how.empty() && a.have_installed) {
        a.keep_saves = how != L"all";
        a.left_behind = setup::Uninstall(a.installed, a.keep_saves);
        a.page = Page::Uninstalled;
        a.auto_run = true;
    }

    // ---- the window
    const bool hidden = !util::Env(L"XMD_SETUP_HIDDEN").empty();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"XMDSetupWindow";
    RegisterClassExW(&wc);
    {
        const HDC dc = GetDC(nullptr);
        g_ui = std::clamp(float(GetDeviceCaps(dc, LOGPIXELSX)) / 96.0f, 1.0f, 3.0f);
        ReleaseDC(nullptr, dc);
    }
    RECT r{0, 0, LONG(960 * g_ui), LONG(700 * g_ui)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"X-Men Destiny Setup", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) // (no graphics driver: Windows' software renderer still shows the setup)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"The setup's window could not be created (Direct3D 11 is not available).", L"X-Men Destiny Setup", MB_ICONERROR);
        JoinAll(a);
        return 4;
    }
    CreateTarget();
    DragAcceptFiles(g_hwnd, TRUE);
    if (!hidden) {
        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    Style();
    LoadFonts();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    const std::wstring shot = util::Env(L"XMD_SETUP_SHOT");
    int frames = 0;
    while (!a.quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                a.quit = true;
        }
        if (a.quit)
            break;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame(a);
        ImGui::Render();
        const float clear[4] = {0.11f, 0.12f, 0.14f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        frames++;
        // a picture for the tests: once the page has settled (scans done, then a few frames), or after a while
        static int settled = 0;
        settled = (!a.scanning && a.tools_checked) ? settled + 1 : 0;
        if (!shot.empty() && ((frames > 40 && settled > 20) || frames > 3000)) {
            SavePng(shot);
            a.quit = true;
        }
        if (a.auto_run && a.page == Page::Uninstalled)
            a.quit = true;
        g_swap->Present(hidden ? 0 : 1, 0);
        if (hidden)
            Sleep(16);
    }

    const bool uninstalled = a.page == Page::Uninstalled;
    const fs::path install = a.installed.dir;
    JoinAll(a);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(g_hwnd);
    if (uninstalled && as_launcher)
        setup::RemoveSelfLater(install);
    else if (uninstalled) {
        fs::remove(install / setup::kLauncherExe, ec);
        fs::remove(install, ec); // (only when empty)
    }
    CoUninitialize();
    return a.progress.finished && !a.progress.ok ? 4 : 0;
}

} // namespace wizard
