#include "ui/Widgets.h"

#include "ui/Icons.h"
#include "ui/Theme.h"
#include "util/Strings.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <format>
#include <numbers>

namespace ps::ui {

using namespace ps::theme;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

ImVec2 operator+(ImVec2 a, ImVec2 b) { return { a.x + b.x, a.y + b.y }; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return { a.x - b.x, a.y - b.y }; }
ImVec2 operator*(ImVec2 a, float s) { return { a.x * s, a.y * s }; }

bool StyledButton(const char* label, const ImVec2& size, bool enabled, const ImVec4& bg, const ImVec4& bgHover,
    const ImVec4& text, bool strong)
{
    if (!enabled)
        ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(bgHover, Text, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(8)));
    if (strong)
        ImGui::PushFont(GetFonts().uiStrong, 0.0f);
    const bool pressed = ImGui::Button(label, size);
    if (strong)
        ImGui::PopFont();
    if (enabled && ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (!enabled)
        ImGui::EndDisabled();
    return pressed && enabled;
}

struct CardFrame {
    ImVec2 start;
    float width = 0;
    bool clickable = false;
    const char* id = nullptr;
};
CardFrame g_card;
bool g_inCard = false;

struct ToastItem {
    std::string text;
    ToastKind kind;
    double start;
};
std::deque<ToastItem> g_toasts;

} // namespace

// ---- Text ------------------------------------------------------------------

void DimText(std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, TextDim);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

void ColoredText(const ImVec4& col, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

float TextWidth(std::string_view text) { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; }

void TextCentered(std::string_view text, const ImVec4* col)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = TextWidth(text);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - w) * 0.5f));
    if (col)
        ColoredText(*col, text);
    else
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

// ---- Buttons ---------------------------------------------------------------

bool PrimaryButton(const char* label, const ImVec2& size, bool enabled)
{
    return StyledButton(label, size, enabled, Accent, Mix(Accent, Text, 0.18f), Background, true);
}

bool SecondaryButton(const char* label, const ImVec2& size, bool enabled)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool r = StyledButton(label, size, enabled, PanelRaised, PanelHover, Text, false);
    ImGui::PopStyleVar();
    return r;
}

bool DangerButton(const char* label, const ImVec2& size, bool enabled)
{
    return StyledButton(label, size, enabled, Danger, Mix(Danger, Text, 0.15f), Hex(0x1A0B0B), true);
}

bool GhostButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PanelHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, PanelRaised);
    ImGui::PushStyleColor(ImGuiCol_Text, TextDim);
    // Re-colour text on hover: draw the button, then check hover for next frame.
    const ImGuiID id = ImGui::GetID(label);
    const bool hoveredLast = ImGui::GetCurrentContext()->HoveredIdPreviousFrame == id;
    if (hoveredLast)
        ImGui::PushStyleColor(ImGuiCol_Text, Text);
    const bool pressed = ImGui::Button(label, size);
    if (hoveredLast)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopStyleColor(4);
    return pressed;
}

// ---- Small pieces ----------------------------------------------------------

void Pill(std::string_view text, const ImVec4& color, bool filled)
{
    Font f(GetFonts().uiStrong, FontSmall - 1.0f);
    const ImVec2 pad(S(7), S(2));
    const ImVec2 ts = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, U32(color, filled ? 1.0f : 0.16f), size.y * 0.5f);
    dl->AddText(p + pad, filled ? U32(Background) : U32(color), text.data(), text.data() + text.size());
    ImGui::Dummy(size);
}

ImVec4 StorageColor(double f)
{
    if (f > 0.90)
        return Danger;
    if (f >= 0.75)
        return Warning;
    return Accent;
}

void StorageBar(double usedFraction, float width, float height)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = height * 0.5f;
    dl->AddRectFilled(p, p + ImVec2(width, height), U32(Border), r);
    const float fw = std::clamp(static_cast<float>(usedFraction), 0.0f, 1.0f) * width;
    if (fw > 0.5f)
        dl->AddRectFilled(p, p + ImVec2(std::max(fw, height), height), U32(StorageColor(usedFraction)), r);
    ImGui::Dummy(ImVec2(width, height));
}

void ProgressBarThin(float fraction, float width, float height, const ImVec4& color)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(width, height), U32(Border), height * 0.5f);
    const float fw = std::clamp(fraction, 0.0f, 1.0f) * width;
    if (fw > 0.5f)
        dl->AddRectFilled(p, p + ImVec2(fw, height), U32(color), height * 0.5f);
    ImGui::Dummy(ImVec2(width, height));
}

void Spinner(float radius, float thickness, const ImVec4& color)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c = p + ImVec2(radius, radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = static_cast<float>(ImGui::GetTime());
    dl->AddCircle(c, radius - thickness * 0.5f, U32(color, 0.18f), 24, thickness);
    const float a0 = t * 5.0f;
    const float sweep = kPi * (0.6f + 0.35f * std::sin(t * 2.2f));
    dl->PathArcTo(c, radius - thickness * 0.5f, a0, a0 + sweep, 20);
    dl->PathStroke(U32(color), 0, thickness);
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

void HLine(float alpha)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Inside a card, stop at the card's inner edge rather than the window's.
    const float w = g_inCard ? g_card.start.x + g_card.width - S(PadLarge + 4) - p.x : ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(p, p + ImVec2(w, 0), U32(Border, alpha), 1.0f);
    ImGui::Dummy(ImVec2(w, 1.0f));
}

void EmptyState(const char* icon, std::string_view title, std::string_view subtitle)
{
    ImGui::Dummy(ImVec2(0, S(24)));
    {
        Font f(GetFonts().ui, 34.0f);
        TextCentered(icon, &TextDim);
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        TextCentered(title);
    }
    if (!subtitle.empty())
        TextCentered(subtitle, &TextDim);
}

// ---- Cards -----------------------------------------------------------------

void BeginCard(const char* id, float width, bool clickable)
{
    IM_ASSERT(!g_inCard && "Cards don't nest");
    g_inCard = true;
    g_card = { ImGui::GetCursorScreenPos(), width, clickable, id };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    const float pad = S(PadLarge + 4);
    ImGui::SetCursorScreenPos(g_card.start + ImVec2(pad, pad));
    ImGui::BeginGroup();
    // PushTextWrapPos wants window-local X, not screen X.
    const float localRight = g_card.start.x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + width - pad;
    ImGui::PushTextWrapPos(localRight);
    ImGui::PushItemWidth(width - pad * 2);
}

CardResult EndCard()
{
    IM_ASSERT(g_inCard);
    g_inCard = false;
    ImGui::PopItemWidth();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    const float pad = S(PadLarge + 4);
    CardResult r;
    r.min = g_card.start;
    r.max = ImVec2(g_card.start.x + g_card.width, ImGui::GetItemRectMax().y + pad);

    ImGui::SetCursorScreenPos(r.min);
    if (g_card.clickable) {
        // Submitted after the content, so inner buttons keep hover priority.
        r.clicked = ImGui::InvisibleButton(g_card.id, r.max - r.min);
        r.hovered = ImGui::IsItemHovered();
        if (r.hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else {
        ImGui::Dummy(r.max - r.min);
        r.hovered = ImGui::IsMouseHoveringRect(r.min, r.max);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    const bool lift = g_card.clickable && r.hovered;
    if (lift) // soft shadow under a raised card
        dl->AddRectFilled(r.min + ImVec2(0, S(4)), r.max + ImVec2(0, S(6)), U32Raw(Hex(0x000000), 0.35f), S(RoundPanel + 2));
    dl->AddRectFilled(r.min, r.max, U32(lift ? PanelHover : PanelRaised), S(RoundPanel));
    dl->AddRect(r.min, r.max, U32(lift ? Accent : Border, lift ? 0.9f : 1.0f), S(RoundPanel), 0, lift ? 1.5f : 1.0f);
    dl->ChannelsMerge();
    return r;
}

// ---- Phone illustration ----------------------------------------------------

void DrawPhone(ImDrawList* dl, ImVec2 center, float h, PhoneArt art, double timeD)
{
    const float t = static_cast<float>(timeD);
    const float w = h * 0.52f;
    const ImVec2 min = center - ImVec2(w * 0.5f, h * 0.5f);
    const ImVec2 max = center + ImVec2(w * 0.5f, h * 0.5f);
    const float rounding = w * 0.16f;
    const float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);

    // Cable (behind the phone so it tucks under the bottom edge).
    {
        const float cableTop = max.y - S(2);
        const float cableLen = h * 0.42f;
        const float cx = center.x;
        const float thick = std::max(2.0f, h * 0.022f);
        if (art == PhoneArt::Waiting) {
            // Animated dashes flowing toward the phone: "plug me in".
            const float dash = h * 0.04f, gap = h * 0.035f, period = dash + gap;
            const float offset = std::fmod(t * h * 0.12f, period);
            for (float y = cableTop + cableLen + offset - period; y > cableTop - period; y -= period) {
                const float y0 = std::max(y - dash, cableTop + h * 0.06f);
                const float y1 = std::min(y, cableTop + cableLen);
                if (y1 > y0)
                    dl->AddLine(ImVec2(cx, y0), ImVec2(cx, y1), U32(TextDim, 0.55f), thick);
            }
            // Plug floating just below the phone, bobbing.
            const float bob = std::sin(t * 2.4f) * h * 0.01f;
            const ImVec2 plugMin(cx - w * 0.09f, cableTop + h * 0.05f + bob);
            const ImVec2 plugMax(cx + w * 0.09f, cableTop + h * 0.12f + bob);
            dl->AddRectFilled(plugMin, plugMax, U32(Border), S(3));
            dl->AddRectFilled(ImVec2(cx - w * 0.05f, plugMin.y - h * 0.025f), ImVec2(cx + w * 0.05f, plugMin.y + 1), U32(TextDim, 0.7f), S(1.5f));
        } else {
            dl->AddLine(ImVec2(cx, cableTop), ImVec2(cx, cableTop + cableLen), U32(art == PhoneArt::Ready ? Accent : TextDim, 0.6f), thick);
            dl->AddRectFilled(ImVec2(cx - w * 0.09f, cableTop - 1), ImVec2(cx + w * 0.09f, cableTop + h * 0.06f), U32(Border), S(3));
        }
    }

    // Waiting: ripple rings expanding from the body.
    if (art == PhoneArt::Waiting) {
        for (int i = 0; i < 2; ++i) {
            float k = std::fmod(t * 0.45f + i * 0.5f, 1.0f);
            const float grow = k * h * 0.16f;
            dl->AddRect(min - ImVec2(grow, grow), max + ImVec2(grow, grow), U32(Accent, (1.0f - k) * 0.28f), rounding + grow, 0, 1.5f);
        }
    }

    // Body.
    const ImVec4 rim = art == PhoneArt::Waiting ? Mix(Border, Accent, 0.25f + 0.35f * pulse)
        : art == PhoneArt::Ready                 ? Accent
                                                 : Mix(Border, Warning, 0.5f);
    dl->AddRectFilled(min, max, U32(PanelRaised), rounding);
    dl->AddRect(min, max, U32(rim), rounding, 0, std::max(1.5f, h * 0.014f));

    // Screen.
    const float inset = w * 0.07f;
    const ImVec2 smin = min + ImVec2(inset, inset * 1.4f);
    const ImVec2 smax = max - ImVec2(inset, inset * 1.4f);
    dl->AddRectFilled(smin, smax, U32(Background), rounding * 0.6f);
    // Punch-hole camera.
    dl->AddCircleFilled(ImVec2(center.x, smin.y + h * 0.035f), h * 0.012f, U32(Border));

    ImFont* icons = GetFonts().ui;
    auto glyph = [&](const char* g, float size, ImVec2 c, ImU32 col) {
        const ImVec2 ts = icons->CalcTextSizeA(size, FLT_MAX, 0.0f, g);
        dl->AddText(icons, size, c - ts * 0.5f, col, g);
    };

    switch (art) {
    case PhoneArt::Waiting:
        glyph(ICON_PLUG, h * 0.16f, center, U32(TextDim, 0.35f + 0.35f * pulse));
        break;
    case PhoneArt::Ready:
        glyph(ICON_CIRCLE_CHECK, h * 0.2f, center, U32(Accent));
        break;
    case PhoneArt::Unauthorized: {
        // A mini "Allow USB debugging?" dialog on the phone screen.
        const float dw = (smax.x - smin.x) * 0.86f;
        const float dh = h * 0.30f;
        const ImVec2 dmin(center.x - dw * 0.5f, center.y - dh * 0.5f);
        const ImVec2 dmax(center.x + dw * 0.5f, center.y + dh * 0.5f);
        dl->AddRectFilled(dmin, dmax, U32(Panel), S(4));
        dl->AddRect(dmin, dmax, U32(Border), S(4));
        const float lx = dmin.x + dw * 0.1f, lw = dw * 0.8f, lh = std::max(2.0f, h * 0.014f);
        dl->AddRectFilled(ImVec2(lx, dmin.y + dh * 0.14f), ImVec2(lx + lw * 0.75f, dmin.y + dh * 0.14f + lh * 1.4f), U32(Text, 0.8f), lh);
        dl->AddRectFilled(ImVec2(lx, dmin.y + dh * 0.32f), ImVec2(lx + lw, dmin.y + dh * 0.32f + lh), U32(TextDim, 0.6f), lh);
        dl->AddRectFilled(ImVec2(lx, dmin.y + dh * 0.44f), ImVec2(lx + lw * 0.6f, dmin.y + dh * 0.44f + lh), U32(TextDim, 0.6f), lh);
        // Checkbox line ("Always allow").
        const float cb = h * 0.022f;
        dl->AddRect(ImVec2(lx, dmin.y + dh * 0.58f), ImVec2(lx + cb, dmin.y + dh * 0.58f + cb), U32(Accent), 1.0f);
        dl->AddRectFilled(ImVec2(lx + cb * 1.6f, dmin.y + dh * 0.58f + cb * 0.3f), ImVec2(lx + lw * 0.7f, dmin.y + dh * 0.58f + cb * 0.3f + lh), U32(TextDim, 0.5f), lh);
        // Buttons: Deny (dim) / Allow (accent, pulsing).
        const float by = dmax.y - dh * 0.22f, bh = dh * 0.13f, bw = dw * 0.32f;
        dl->AddRectFilled(ImVec2(dmax.x - dw * 0.1f - bw * 2 - dw * 0.04f, by), ImVec2(dmax.x - dw * 0.1f - bw - dw * 0.04f, by + bh), U32(Border), bh * 0.5f);
        const ImVec2 amin(dmax.x - dw * 0.1f - bw, by), amax(dmax.x - dw * 0.1f, by + bh);
        dl->AddRectFilled(amin, amax, U32(Accent, 0.7f + 0.3f * pulse), bh * 0.5f);
        const float g = pulse * h * 0.02f;
        dl->AddRect(amin - ImVec2(g, g), amax + ImVec2(g, g), U32(Accent, (1.0f - pulse) * 0.6f), bh * 0.5f + g, 0, 1.5f);
        break;
    }
    }
}

// ---- Device card -----------------------------------------------------------

namespace {

const char* BatteryIcon(int level)
{
    if (level >= 88) return ICON_BATTERY_FULL;
    if (level >= 63) return ICON_BATTERY_3Q;
    if (level >= 38) return ICON_BATTERY_HALF;
    if (level >= 13) return ICON_BATTERY_1Q;
    return ICON_BATTERY_EMPTY;
}

std::string GB(uint64_t bytes) { return std::format("{:.0f}", static_cast<double>(bytes) / 1e9); }
const char* Or(const std::string& s) { return s.empty() ? "\xE2\x80\x94" : s.c_str(); } // em dash

// valueX is window-local. SameLine(x) would be relative to the group indent,
// so set the cursor explicitly instead.
void LabelRow(const char* label, float valueX)
{
    ImGui::AlignTextToFramePadding();
    DimText(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(valueX);
}

} // namespace

DeviceCardAction DeviceCard(const DeviceInfo& d, float width, const DeviceCardOptions& opts)
{
    DeviceCardAction action = DeviceCardAction::None;
    ImGui::PushID(d.serial.c_str());
    BeginCard("##card", width, opts.clickable);

    const float inner = width - S(PadLarge + 4) * 2;

    // Header: icon + name + model/serial.
    {
        const ImVec2 iconPos = ImGui::GetCursorScreenPos();
        const float iconBox = S(44);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(iconPos, iconPos + ImVec2(iconBox, iconBox), U32(Accent, 0.12f), S(RoundPanel));
        {
            Font f(GetFonts().ui, 22.0f);
            const ImVec2 ts = ImGui::CalcTextSize(ICON_PHONE);
            dl->AddText(iconPos + (ImVec2(iconBox, iconBox) - ts) * 0.5f, U32(Accent), ICON_PHONE);
        }
        ImGui::Dummy(ImVec2(iconBox, iconBox));
        ImGui::SameLine(0, S(12));
        ImGui::BeginGroup();
        {
            Font f(GetFonts().uiStrong, FontH2 - 2.0f);
            ImGui::TextUnformatted(d.displayName.c_str());
        }
        {
            Font f(GetFonts().mono, FontSmall);
            const bool reveal = opts.revealSerial && *opts.revealSerial;
            const std::string serial = reveal ? d.serial : MaskSerial(d.serial);
            DimText(std::format("{} \xC2\xB7 {}", Or(d.model), serial));
            if (opts.revealSerial) {
                ImGui::SameLine(0, S(4));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), 0));
                if (GhostButton(reveal ? ICON_EYE_SLASH "##eye" : ICON_EYE "##eye"))
                    *opts.revealSerial = !*opts.revealSerial;
                ImGui::PopStyleVar();
                ImGui::SetItemTooltip(reveal ? "Hide serial number" : "Show serial number");
            }
        }
        ImGui::EndGroup();
    }

    ImGui::Dummy(ImVec2(0, S(4)));
    HLine(0.8f);
    ImGui::Dummy(ImVec2(0, S(2)));

    const float labelW = S(96);
    const float startX = ImGui::GetCursorPosX();
    const float col = startX + labelW;

    // System
    LabelRow("System", col);
    {
        std::string sys = std::format("Android {}", Or(d.android));
        if (!d.oneUi.empty())
            sys += std::format("  \xC2\xB7  One UI {}", d.oneUi);
        if (!d.securityPatch.empty())
            sys += std::format("  \xC2\xB7  Patch {}", d.securityPatch);
        ImGui::TextUnformatted(sys.c_str());
    }

    // Battery
    LabelRow("Battery", col);
    if (d.batteryLevel) {
        const int lvl = *d.batteryLevel;
        const ImVec4 bc = lvl <= 15 ? Danger : lvl <= 30 ? Warning : Text;
        ColoredText(bc, std::format("{} {}%", BatteryIcon(lvl), lvl));
        if (d.charging) {
            ImGui::SameLine();
            ColoredText(Accent, ICON_BOLT " charging");
        }
        if (d.batteryTempC) {
            ImGui::SameLine();
            DimText(std::format("\xC2\xB7  {:.1f}\xC2\xB0" "C", *d.batteryTempC));
        }
    } else {
        DimText("\xE2\x80\x94");
    }

    // RAM
    LabelRow("RAM", col);
    if (d.ramMarketingGB)
        ImGui::Text("%d GB", *d.ramMarketingGB);
    else
        DimText("\xE2\x80\x94");

    // Storage
    LabelRow("Storage", col);
    if (d.storage && d.storage->total) {
        const double f = d.storage->UsedFraction();
        const float barW = inner - labelW - S(96);
        // Bar and "used / total" both centred on the row's frame-height line.
        const float rowY = ImGui::GetCursorPosY();
        const float frameH = ImGui::GetFrameHeight();
        ImGui::SetCursorPosY(rowY + (frameH - S(10)) * 0.5f);
        StorageBar(f, barW, S(10));
        ImGui::SameLine(0, S(10));
        // The line already carries the label's frame-padding baseline offset, so
        // returning to rowY is enough to centre the text.
        ImGui::SetCursorPosY(rowY);
        ImGui::TextUnformatted(std::format("{} / {} GB", GB(d.storage->used), GB(d.storage->total)).c_str());
        ImGui::SetCursorPosX(col);
        Font fs(GetFonts().ui, FontSmall);
        ColoredText(f >= 0.75 ? StorageColor(f) : TextDim,
            std::format("{:.0f}% used \xC2\xB7 {} free", f * 100.0, FormatBytes(d.storage->free)));
    } else {
        DimText("\xE2\x80\x94");
    }

    // Connection
    LabelRow("Connection", col);
    ImGui::TextUnformatted(d.wireless ? ICON_WIFI "  Wi-Fi" : ICON_PLUG "  USB");

    if (opts.showProceed) {
        ImGui::Dummy(ImVec2(0, S(6)));
        const char* label = "Proceed  " ICON_ARROW_RIGHT;
        const float bw = TextWidth(label) + S(28) + S(24);
        ImGui::SetCursorPosX(startX + inner - bw);
        if (PrimaryButton(label, ImVec2(bw, 0)))
            action = DeviceCardAction::Proceed;
    }

    const CardResult r = EndCard();
    if (r.clicked && action == DeviceCardAction::None)
        action = DeviceCardAction::Clicked;
    ImGui::PopID();
    return action;
}

// ---- Data display ----------------------------------------------------------

namespace {
bool g_binaryUnits = false;
}

void SetBinaryUnits(bool binary) { g_binaryUnits = binary; }
std::string Bytes(uint64_t bytes) { return FormatBytes(bytes, g_binaryUnits); }

std::string Count(uint64_t n)
{
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string Date(int64_t t)
{
    if (t <= 0)
        return "\xE2\x80\x94";
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    if (localtime_s(&tm, &tt) != 0)
        return "\xE2\x80\x94";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

ImVec4 CategoryColor(uint8_t c)
{
    static constexpr ImVec4 kColors[] = { CatImages, CatVideos, CatMusic, CatDocs, CatArchives, CatOther };
    return c < std::size(kColors) ? kColors[c] : CatOther;
}

const char* CategoryIcon(uint8_t c)
{
    static constexpr const char* kIcons[] = { ICON_IMAGE, ICON_FILM, ICON_MUSIC, ICON_FILE_LINES, ICON_FILE_ZIPPER, ICON_FILE };
    return c < std::size(kIcons) ? kIcons[c] : ICON_FILE;
}

void Donut(const DonutSegment* segs, int count, float radius, float thickness, std::string_view centerTop,
    std::string_view centerBottom, float fill)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + radius, p.y + radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = radius - thickness * 0.5f;
    dl->AddCircle(c, r, U32(Border, 0.6f), 96, thickness);

    double total = 0;
    for (int i = 0; i < count; ++i)
        total += std::max(0.0, segs[i].value);
    if (total > 0) {
        const float sweepTotal = 2.0f * kPi * std::clamp(fill, 0.0f, 1.0f);
        const float gap = count > 1 ? 0.025f : 0.0f; // small gap between segments
        float a = -kPi * 0.5f;
        for (int i = 0; i < count; ++i) {
            const float sweep = static_cast<float>(std::max(0.0, segs[i].value) / total) * sweepTotal;
            if (sweep > gap * 1.5f) {
                const int n = std::max(4, static_cast<int>(sweep / (2 * kPi) * 96));
                dl->PathArcTo(c, r, a + gap * 0.5f, a + sweep - gap * 0.5f, n);
                dl->PathStroke(U32(segs[i].color), 0, thickness);
            }
            a += sweep;
        }
    }

    {
        Font f(GetFonts().uiStrong, FontH2);
        const ImVec2 ts = ImGui::CalcTextSize(centerTop.data(), centerTop.data() + centerTop.size());
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.85f), U32(Text), centerTop.data(), centerTop.data() + centerTop.size());
    }
    {
        Font f(GetFonts().ui, FontSmall);
        const ImVec2 ts = ImGui::CalcTextSize(centerBottom.data(), centerBottom.data() + centerBottom.size());
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + S(4)), U32(TextDim), centerBottom.data(), centerBottom.data() + centerBottom.size());
    }
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

void SkeletonRows(int rows, float rowHeight)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float t = static_cast<float>(ImGui::GetTime());
    for (int i = 0; i < rows; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float a = 0.35f + 0.25f * std::sin(t * 3.0f - i * 0.35f);
        const float h = rowHeight * 0.38f;
        const float y = p.y + (rowHeight - h) * 0.5f;
        dl->AddRectFilled(ImVec2(p.x + S(8), y), ImVec2(p.x + S(8) + h, y + h), U32(Border, a), S(3));
        dl->AddRectFilled(ImVec2(p.x + S(36), y), ImVec2(p.x + S(36) + w * (0.25f + 0.1f * ((i * 7) % 3)), y + h), U32(Border, a), S(3));
        dl->AddRectFilled(ImVec2(p.x + w * 0.55f, y), ImVec2(p.x + w * 0.72f, y + h), U32(Border, a * 0.8f), S(3));
        dl->AddRectFilled(ImVec2(p.x + w * 0.80f, y), ImVec2(p.x + w * 0.90f, y + h), U32(Border, a * 0.8f), S(3));
        ImGui::Dummy(ImVec2(w, rowHeight));
    }
}

bool ErrorCard(const char* id, std::string_view message, std::string_view details, const char* retryLabel)
{
    bool retry = false;
    ImGui::PushID(id);
    const float w = std::min(S(620), ImGui::GetContentRegionAvail().x);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
    BeginCard("##err", w);
    ColoredText(Danger, ICON_WARNING);
    ImGui::SameLine();
    ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
    ImGui::Dummy(ImVec2(0, S(4)));
    if (PrimaryButton(retryLabel))
        retry = true;
    if (!details.empty()) {
        ImGui::SameLine();
        static ImGuiStorage open; // per-id expander state
        bool* show = open.GetBoolRef(ImGui::GetID("details"), false);
        if (GhostButton(*show ? ICON_CHEVRON_DOWN "  Hide details" : ICON_CHEVRON_RIGHT "  Details"))
            *show = !*show;
        if (*show) {
            Font f(GetFonts().mono, FontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, TextDim);
            ImGui::TextWrapped("%.*s", static_cast<int>(details.size()), details.data());
            ImGui::PopStyleColor();
        }
    }
    EndCard();
    ImGui::PopID();
    return retry;
}

void BeginPanel(const char* id, const char* title, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(PadLarge), S(14)));
    // Scrolls only if content outgrows the panel (e.g. a long Attention list at 150%).
    // Height 0 = size to content (AutoResizeY); otherwise fixed and scrollable.
    ImGui::BeginChild(id, size,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | (size.y == 0.0f ? ImGuiChildFlags_AutoResizeY : 0));
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(title);
    }
    ImGui::Dummy(ImVec2(0, S(4)));
}

void EndPanel()
{
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---- App logo ----------------------------------------------------------------

namespace {
uint64_t g_logo = 0;
}

void SetLogoTexture(uint64_t texture) { g_logo = texture; }

void UpdateDot(ImVec2 itemMin, ImVec2 itemMax)
{
    const float r = S(4);
    const ImVec2 c(itemMax.x - r - S(1), itemMin.y + r + S(1));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r + S(1.5f), U32(Background));
    dl->AddCircleFilled(c, r, U32(Accent));
}

void Logo(float size)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (g_logo) {
        ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(g_logo)), p, ImVec2(p.x + size, p.y + size),
            ImVec2(0, 0), ImVec2(1, 1), U32Raw(ImVec4(1, 1, 1, ImGui::GetStyle().Alpha)), size * 0.22f);
        ImGui::Dummy(ImVec2(size, size));
    } else {
        Font f(GetFonts().ui, size / Dpi());
        ColoredText(Accent, ICON_PHONE);
    }
}

// ---- Toasts ----------------------------------------------------------------

void Toast(std::string text, ToastKind kind)
{
    g_toasts.push_back({ std::move(text), kind, ImGui::GetTime() });
    if (g_toasts.size() > 4)
        g_toasts.pop_front();
}

void DrawToasts(float bottomMargin)
{
    constexpr double kLife = 3.0, kFade = 0.25;
    const double now = ImGui::GetTime();
    std::erase_if(g_toasts, [&](const ToastItem& t) { return now - t.start > kLife; });
    if (g_toasts.empty())
        return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 vp = ImGui::GetMainViewport()->WorkPos + ImGui::GetMainViewport()->WorkSize;
    float y = vp.y - bottomMargin - S(12);
    Font f(GetFonts().ui, FontBase);
    for (auto it = g_toasts.rbegin(); it != g_toasts.rend(); ++it) {
        const double age = now - it->start;
        const float a = static_cast<float>(std::min({ 1.0, age / kFade, (kLife - age) / kFade }));
        const ImVec4 accent = it->kind == ToastKind::Success ? Success
            : it->kind == ToastKind::Warning               ? Warning
            : it->kind == ToastKind::Error                 ? Danger
                                                           : Info;
        const char* icon = it->kind == ToastKind::Success ? ICON_CIRCLE_CHECK
            : it->kind == ToastKind::Info                 ? ICON_INFO
                                                          : ICON_WARNING;
        const std::string line = std::format("{}  {}", icon, it->text);
        const ImVec2 ts = ImGui::CalcTextSize(line.c_str());
        const ImVec2 pad(S(14), S(10));
        const ImVec2 size = ts + pad * 2;
        const ImVec2 min(vp.x - size.x - S(16), y - size.y + (1.0f - a) * S(8));
        dl->AddRectFilled(min + ImVec2(0, S(3)), min + size + ImVec2(0, S(5)), U32Raw(Hex(0), 0.3f * a), S(RoundPanel));
        dl->AddRectFilled(min, min + size, U32Raw(PanelRaised, a), S(RoundPanel));
        dl->AddRect(min, min + size, U32Raw(Border, a), S(RoundPanel));
        dl->AddRectFilled(min + ImVec2(0, S(6)), ImVec2(min.x + S(3), min.y + size.y - S(6)), U32Raw(accent, a), S(2));
        dl->AddText(min + pad, U32Raw(Text, a), line.c_str());
        y -= size.y + S(8);
    }
}

bool Radio(const char* label, bool active)
{
    ImGui::PushStyleColor(ImGuiCol_CheckMark, Accent);
    const bool pressed = ImGui::RadioButton(label, active);
    ImGui::PopStyleColor();
    return pressed;
}

bool Tabs(int& tab, std::initializer_list<const char*> labels)
{
    const int before = tab;
    int idx = 0;
    for (const char* label : labels) {
        if (idx > 0)
            ImGui::SameLine(0, S(4));
        const bool active = tab == idx;
        ImGui::PushID(idx);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? PanelRaised : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PanelHover);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Text : TextDim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(7)));
        if (ImGui::Button(label))
            tab = idx;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopID();
        if (active) {
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x + S(8), p1.y - S(2)), ImVec2(p1.x - S(8), p1.y), U32(Accent), S(1));
        }
        ++idx;
    }
    return tab != before;
}

} // namespace ps::ui
