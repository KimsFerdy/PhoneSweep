#pragma once
#include <imgui.h>

#include <filesystem>

namespace ps::theme {

// ---- Palette (UI spec §1) ----------------------------------------------------

constexpr ImVec4 Hex(unsigned rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

inline constexpr ImVec4 Background = Hex(0x0F1115);
inline constexpr ImVec4 Panel = Hex(0x161A20);
inline constexpr ImVec4 PanelRaised = Hex(0x1D222A);
inline constexpr ImVec4 PanelHover = Hex(0x232933);
inline constexpr ImVec4 Border = Hex(0x2A303A);
inline constexpr ImVec4 Text = Hex(0xE6E9EF);
inline constexpr ImVec4 TextDim = Hex(0x8A93A3);

inline constexpr ImVec4 Accent = Hex(0x3DD6C6);
inline constexpr ImVec4 Info = Hex(0x5AA9FF);
inline constexpr ImVec4 Success = Hex(0x4CD07D);
inline constexpr ImVec4 Warning = Hex(0xF2B84B);
inline constexpr ImVec4 Danger = Hex(0xFF5C5C); // Nuke / delete / uninstall only

inline constexpr ImVec4 CatImages = Hex(0x5AA9FF);
inline constexpr ImVec4 CatVideos = Hex(0xB07CFF);
inline constexpr ImVec4 CatMusic = Hex(0xFF8FB1);
inline constexpr ImVec4 CatDocs = Hex(0xF2B84B);
inline constexpr ImVec4 CatArchives = Hex(0x4CD07D);
inline constexpr ImVec4 CatOther = Hex(0x8A93A3);
inline constexpr ImVec4 CatApps = Hex(0x3DD6C6);

inline ImU32 U32(const ImVec4& c, float alphaMul = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul)); }
// Same as U32 but ignores style.Alpha (for things drawn during fades that must stay solid).
inline ImU32 U32Raw(const ImVec4& c, float alphaMul = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul)); }
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t);

// ---- Metrics (unscaled px; multiply by Dpi()) -------------------------------

inline constexpr float FontBase = 15.0f;
inline constexpr float FontSmall = 13.0f;
inline constexpr float FontH2 = 22.0f;
inline constexpr float FontH1 = 30.0f;

inline constexpr float RoundPanel = 8.0f;
inline constexpr float RoundButton = 6.0f;
inline constexpr float Pad = 14.0f;
inline constexpr float PadLarge = 16.0f;
inline constexpr float SidebarWidth = 200.0f; // spec says ~180; "Background & Startup" needs a bit more
inline constexpr float SidebarCollapsedWidth = 56.0f;
inline constexpr float SidebarCollapseBelow = 1100.0f;
inline constexpr float TopBarHeight = 48.0f;
inline constexpr float StatusBarHeight = 30.0f;

// ---- Fonts -----------------------------------------------------------------

struct Fonts {
    ImFont* ui = nullptr;       // Inter Regular + icons
    ImFont* uiStrong = nullptr; // Inter SemiBold + icons (headings, buttons)
    ImFont* mono = nullptr;     // JetBrains Mono + icons (paths, packages, logs)
};

// Loads fonts from `dir`. Falls back to ImGui's default font if files are missing.
Fonts LoadFonts(const std::filesystem::path& dir);
const Fonts& GetFonts();

// Applies colours/metrics for a given scale (DPI scale x user UI scale).
void Apply(float scale);
float Dpi(); // current scale
inline float S(float px) { return px * Dpi(); }

// RAII font push: Font f(GetFonts().uiStrong, FontH2);
class Font {
public:
    Font(ImFont* font, float unscaledSize) { ImGui::PushFont(font, unscaledSize); }
    ~Font() { ImGui::PopFont(); }
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
};

} // namespace ps::theme
