// PhoneSweep entry point: Win32 + DX11 + Dear ImGui bootstrap.
//
// Dev flags (handy for UI sign-off without a phone):
//   --fake=noadb|none|unauthorized|one|two|three   fake device scenario
//   --screenshot=out.png                           render, save PNG, exit
//   --frames=N                                     frames before screenshot (default 60)
//   --scale=1.5                                    force UI scale
//   --size=1280x800                                window size (unscaled)
//   --workspace[=page]                             open the first device's workspace
//   --log                                          show the log panel
//   --files=grid,images,camera,preview             Files page preset (view, category, source, open drawer)

#include "app/App.h"
#include "app/Settings.h"
#include "app/Version.h"
#include "apps/M10SelfTest.h"
#include "backup/M9SelfTest.h"
#include "compress/M8SelfTest.h"
#include "nuke/NukeSelfTest.h"
#include "platform/AudioPlayer.h"
#include "platform/Platform.h"
#include "thumbs/ImageOps.h"
#include "ui/Widgets.h"
#include "ui/Theme.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <imgui.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <string>
#include <vector>

namespace {

struct CmdLine {
    ps::AppOptions app;
    std::filesystem::path screenshot;
    int frames = 60;
    int width = 1280, height = 800;
};

ps::Page ParsePage(std::string_view s)
{
    if (s == "files") return ps::Page::Files;
    if (s == "cleanup") return ps::Page::Cleanup;
    if (s == "apps") return ps::Page::Apps;
    if (s == "background") return ps::Page::Background;
    if (s == "queue") return ps::Page::SaveQueue;
    if (s == "nuke") return ps::Page::Nuke;
    if (s == "reports") return ps::Page::Reports;
    return ps::Page::Overview;
}

CmdLine ParseArgs()
{
    CmdLine c;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::string a = ps::Narrow(argv[i]);
        auto value = [&](std::string_view key) -> std::optional<std::string> {
            if (a.starts_with(key) && a.size() > key.size() && a[key.size()] == '=')
                return a.substr(key.size() + 1);
            return std::nullopt;
        };
        if (auto v = value("--fake"))
            c.app.fake = ps::ParseFakeScenario(*v);
        else if (auto v2 = value("--screenshot"))
            c.screenshot = std::filesystem::path(ps::Widen(*v2));
        else if (auto v3 = value("--frames"))
            c.frames = std::max(1, std::atoi(v3->c_str()));
        else if (auto v4 = value("--scale"))
            c.app.forcedScale = static_cast<float>(std::atof(v4->c_str()));
        else if (auto v5 = value("--size"))
            sscanf_s(v5->c_str(), "%dx%d", &c.width, &c.height);
        else if (auto v6 = value("--workspace")) {
            c.app.startRoute = ps::Route::Workspace;
            c.app.startPage = ParsePage(*v6);
        } else if (a == "--workspace")
            c.app.startRoute = ps::Route::Workspace;
        else if (auto v7 = value("--files"))
            c.app.filesPreset = *v7;
        else if (auto v8 = value("--backup-root"))
            c.app.backupRootOverride = *v8;
        else if (auto v9 = value("--settings"))
            c.app.openSettingsTab = std::atoi(v9->c_str());
        else if (auto v10 = value("--pretend-version"))
            c.app.pretendVersion = *v10;
        else if (auto v11 = value("--update-kind"))
            c.app.updateKind = *v11;
        else if (a == "--update-selftest") {
            c.app.updateSelfTest = true;
            if (c.app.pretendVersion.empty())
                c.app.pretendVersion = "0.9.0";
        }
        else if (a == "--log")
            c.app.startWithLog = true;
    }
    LocalFree(argv);
    return c;
}

std::filesystem::path ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    CmdLine args = ParseArgs();
    args.app.noUpdateCheck = !args.screenshot.empty() && args.app.pretendVersion.empty();
    if (!args.screenshot.empty()) {
        // Headless-ish runs must never block on an assert dialog: print and abort instead.
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
    const auto dataDir = ps::Settings::DataDir();
    ps::Log::Get().Open(dataDir / L"logs");
    ps::LogInfo("{} starting", ps::kAppNameVersion);

    // QA: exercise the real nuke executor on throwaway files only, then exit.
    if (std::wstring_view(GetCommandLineW()).find(L"--nuke-selftest") != std::wstring_view::npos)
        return ps::RunNukeSelfTest();
    if (std::wstring_view(GetCommandLineW()).find(L"--m8-selftest") != std::wstring_view::npos)
        return ps::RunM8SelfTest();
    for (int i = 1; i < __argc; ++i) {
        const std::wstring_view a = __wargv[i];
        if (a.starts_with(L"--play-test=")) { // dev: can Media Foundation play this file?
            ps::AudioPlayer player;
            const bool started = player.Play(std::filesystem::path(a.substr(12)), "t");
            bool failed = false;
            const DWORD until = GetTickCount() + 2500;
            while (GetTickCount() < until && !failed) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                failed = player.TakeFailure();
                Sleep(10);
            }
            ps::LogInfo("play-test: started={} failed={} playing={} pos={} ms length={} ms", started, failed, player.IsPlaying(), player.PositionMs(), player.LengthMs());
            return started && !failed && player.PositionMs() > 0 ? 0 : 3;
        }
        if (a.starts_with(L"--m10-selftest"))
            return ps::RunM10SelfTest(a.size() > 15 && a[14] == L'=' ? std::filesystem::path(a.substr(15)) : std::filesystem::path());
        if (a.starts_with(L"--m9-selftest"))
            return ps::RunM9SelfTest(a.size() > 14 && a[13] == L'=' ? std::filesystem::path(a.substr(14)) : std::filesystem::path());
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    const std::string iniPath = ps::Narrow((dataDir / L"imgui.ini").wstring());
    io.IniFilename = args.screenshot.empty() ? iniPath.c_str() : nullptr; // screenshots never touch user layout
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    auto platform = ps::CreateWin32Dx11Platform();
    ps::PlatformConfig pcfg;
    pcfg.width = args.width;
    pcfg.height = args.height;
    if (!platform->Init(pcfg)) {
        MessageBoxW(nullptr, L"PhoneSweep couldn't create a Direct3D 11 window.", L"PhoneSweep", MB_ICONERROR);
        return 1;
    }

    ps::theme::LoadFonts(ExeDir() / L"fonts");

    // App logo for the UI (the window/taskbar icon comes from the .rc resource).
    uint64_t logoTexture = 0;
    {
        std::ifstream in(ExeDir() / L"icons" / L"logo.png", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (auto img = ps::DecodeImage({ reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() })) {
            logoTexture = platform->CreateTexture(img->rgba.data(), img->w, img->h);
            ps::ui::SetLogoTexture(logoTexture);
        }
    }
    auto appPtr = std::make_unique<ps::App>(*platform, args.app);
    ps::App& app = *appPtr;

    int frame = 0;
    int exitCode = 0;
    while (platform->PumpEvents()) {
        if (platform->IsIdle()) {
            Sleep(16);
            continue;
        }
        if (platform->ConsumeDpiChanged())
            app.ApplyScale();

        platform->NewFrame();
        ImGui::NewFrame();
        app.Frame();
        ImGui::Render();
        platform->Render(ps::theme::Background);

        if (!args.screenshot.empty() && ++frame >= args.frames) {
            exitCode = platform->SaveScreenshot(args.screenshot) ? 0 : 2;
            break;
        }
        platform->Present();
    }

    appPtr.reset(); // releases thumbnail textures while the D3D device still exists
    ps::ui::SetLogoTexture(0);
    platform->DestroyTexture(logoTexture);
    platform->Shutdown();
    ImGui::DestroyContext();
    ps::LogInfo("PhoneSweep exiting");
    return exitCode;
}
