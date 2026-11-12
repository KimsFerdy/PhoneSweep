#include "ui/Theme.h"

#include "util/Log.h"
#include "util/Utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ps::theme {

namespace {
Fonts g_fonts;
float g_scale = 1.0f;
} // namespace

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

const Fonts& GetFonts() { return g_fonts; }
float Dpi() { return g_scale; }

Fonts LoadFonts(const std::filesystem::path& dir)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    auto load = [&](const wchar_t* file) -> ImFont* {
        const auto path = dir / file;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            LogWarn("Font missing: {}", Narrow(path.wstring()));
            return nullptr;
        }
        ImFontConfig cfg;
        cfg.OversampleH = 2; // ignored by FreeType, harmless
        ImFont* f = io.Fonts->AddFontFromFileTTF(Narrow(path.wstring()).c_str(), FontBase, &cfg);
        if (!f)
            return nullptr;

        // Merge the icon font into every face so icons can sit inline in any text.
        const auto iconPath = dir / L"fa-solid-900.ttf";
        if (std::filesystem::is_regular_file(iconPath, ec)) {
            ImFontConfig icfg;
            icfg.MergeMode = true;
            icfg.GlyphMinAdvanceX = FontBase * 1.25f; // fixed-width icons line up in lists
            icfg.GlyphOffset = ImVec2(0.0f, 0.5f);
            io.Fonts->AddFontFromFileTTF(Narrow(iconPath.wstring()).c_str(), FontBase * 0.92f, &icfg);
        }

        // System fallbacks for file names: emoji, symbols, and scripts Inter lacks.
        // Glyphs load on demand (ImGui 1.92+), so unused fallbacks cost nothing.
        wchar_t winDir[MAX_PATH];
        if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
            const auto fontsDir = std::filesystem::path(winDir) / L"Fonts";
            for (const wchar_t* fallback : { L"seguiemj.ttf", L"seguisym.ttf", L"segoeui.ttf", L"msyh.ttc", L"Nirmala.ttc" }) {
                const auto fp = fontsDir / fallback;
                if (!std::filesystem::is_regular_file(fp, ec))
                    continue;
                ImFontConfig fcfg;
                fcfg.MergeMode = true;
                io.Fonts->AddFontFromFileTTF(Narrow(fp.wstring()).c_str(), FontBase, &fcfg);
            }
        }
        return f;
    };

    g_fonts.ui = load(L"Inter-Regular.ttf");
    g_fonts.uiStrong = load(L"Inter-SemiBold.ttf");
    g_fonts.mono = load(L"JetBrainsMono-Regular.ttf");

    if (!g_fonts.ui) {
        ImFont* def = io.Fonts->AddFontDefault();
        g_fonts.ui = def;
    }
    if (!g_fonts.uiStrong)
        g_fonts.uiStrong = g_fonts.ui;
    if (!g_fonts.mono)
        g_fonts.mono = g_fonts.ui;
    io.FontDefault = g_fonts.ui;
    return g_fonts;
}

void Apply(float scale)
{
    g_scale = scale;

    ImGuiStyle style; // start from defaults so repeated Apply() calls don't compound scaling
    ImGui::StyleColorsDark(&style);

    style.WindowPadding = ImVec2(Pad, Pad);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = RoundPanel;
    style.ChildRounding = RoundPanel;
    style.PopupRounding = RoundPanel;
    style.FrameRounding = RoundButton;
    style.GrabRounding = RoundButton;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding = RoundButton;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    style.ScaleAllSizes(scale);
    style.FontSizeBase = FontBase;
    style.FontScaleDpi = scale;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Text;
    c[ImGuiCol_TextDisabled] = TextDim;
    c[ImGuiCol_WindowBg] = Background;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = PanelRaised;
    c[ImGuiCol_Border] = Border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = PanelRaised;
    c[ImGuiCol_FrameBgHovered] = PanelHover;
    c[ImGuiCol_FrameBgActive] = Mix(PanelHover, Accent, 0.15f);
    c[ImGuiCol_TitleBg] = Panel;
    c[ImGuiCol_TitleBgActive] = Panel;
    c[ImGuiCol_TitleBgCollapsed] = Panel;
    c[ImGuiCol_MenuBarBg] = Panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Border;
    c[ImGuiCol_ScrollbarGrabHovered] = Mix(Border, TextDim, 0.4f);
    c[ImGuiCol_ScrollbarGrabActive] = TextDim;
    c[ImGuiCol_CheckMark] = Background;
    c[ImGuiCol_CheckboxSelectedBg] = Accent; // filled teal box with a dark tick
    c[ImGuiCol_InputTextCursor] = Accent;
    c[ImGuiCol_TextLink] = Info;
    c[ImGuiCol_SliderGrab] = Accent;
    c[ImGuiCol_SliderGrabActive] = Mix(Accent, Text, 0.3f);
    c[ImGuiCol_Button] = PanelRaised;
    c[ImGuiCol_ButtonHovered] = PanelHover;
    c[ImGuiCol_ButtonActive] = Mix(PanelHover, Accent, 0.2f);
    c[ImGuiCol_Header] = Hex(0x3DD6C6, 0.14f);
    c[ImGuiCol_HeaderHovered] = Hex(0x3DD6C6, 0.10f);
    c[ImGuiCol_HeaderActive] = Hex(0x3DD6C6, 0.20f);
    c[ImGuiCol_Separator] = Border;
    c[ImGuiCol_SeparatorHovered] = Accent;
    c[ImGuiCol_SeparatorActive] = Accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = Hex(0x3DD6C6, 0.4f);
    c[ImGuiCol_ResizeGripActive] = Accent;
    c[ImGuiCol_Tab] = Panel;
    c[ImGuiCol_TabHovered] = PanelHover;
    c[ImGuiCol_TabSelected] = PanelRaised;
    c[ImGuiCol_TabSelectedOverline] = Accent;
    c[ImGuiCol_TabDimmed] = Panel;
    c[ImGuiCol_TabDimmedSelected] = PanelRaised;
    c[ImGuiCol_TabDimmedSelectedOverline] = Border;
    c[ImGuiCol_DockingPreview] = Hex(0x3DD6C6, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = Background;
    c[ImGuiCol_PlotHistogram] = Accent;
    c[ImGuiCol_TableHeaderBg] = Panel;
    c[ImGuiCol_TableBorderStrong] = Border;
    c[ImGuiCol_TableBorderLight] = Hex(0x2A303A, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.015f);
    c[ImGuiCol_TextSelectedBg] = Hex(0x3DD6C6, 0.30f);
    c[ImGuiCol_NavCursor] = Accent;
    c[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.55f);

    ImGui::GetStyle() = style;
}

} // namespace ps::theme
