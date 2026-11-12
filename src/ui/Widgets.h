#pragma once
#include "adb/Device.h"

#include <imgui.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace ps::ui {

// ---- Text ------------------------------------------------------------------

void DimText(std::string_view text);
void ColoredText(const ImVec4& col, std::string_view text);
void TextCentered(std::string_view text, const ImVec4* col = nullptr);
// Same line, vertically centred against a taller item.
float TextWidth(std::string_view text);

// ---- Buttons ---------------------------------------------------------------

bool PrimaryButton(const char* label, const ImVec2& size = {}, bool enabled = true);
bool SecondaryButton(const char* label, const ImVec2& size = {}, bool enabled = true);
bool DangerButton(const char* label, const ImVec2& size = {}, bool enabled = true);
// Borderless, dim text that brightens on hover. For top-bar icons and inline toggles.
bool GhostButton(const char* label, const ImVec2& size = {});

// ---- Small pieces ----------------------------------------------------------

// Rounded pill. filled=false draws a tinted background with coloured text.
void Pill(std::string_view text, const ImVec4& color, bool filled = false);
ImVec4 StorageColor(double usedFraction); // accent <75%, warning 75-90, danger >90
void StorageBar(double usedFraction, float width, float height);
void Spinner(float radius, float thickness, const ImVec4& color);
void ProgressBarThin(float fraction, float width, float height, const ImVec4& color);
void HLine(float alpha = 1.0f); // subtle full-width divider
void EmptyState(const char* icon, std::string_view title, std::string_view subtitle = {});
// Radio button with a visible accent dot (the theme's CheckMark colour is for filled checkboxes).
bool Radio(const char* label, bool active);
// Underlined tab strip (Cleanup, Backup). Returns true when the tab changed.
bool Tabs(int& tab, std::initializer_list<const char*> labels);

// ---- Cards -----------------------------------------------------------------

struct CardResult {
    bool hovered = false;
    bool clicked = false;
    ImVec2 min, max;
};
// Card with padding and a raised background sized to its content.
// clickable=true makes the whole card a button (inner widgets still win hover).
void BeginCard(const char* id, float width, bool clickable = false);
CardResult EndCard();

// ---- Phone illustration (ImDrawList, no assets) ----------------------------

enum class PhoneArt { Waiting, Unauthorized, Ready };
// Draws centred at `center`, `height` tall (screen-space px). Cable drawn below.
void DrawPhone(ImDrawList* dl, ImVec2 center, float height, PhoneArt art, double time);

// ---- Device card -----------------------------------------------------------

enum class DeviceCardAction { None, Proceed, Clicked };
struct DeviceCardOptions {
    bool showProceed = true; // single-device layout
    bool clickable = false;  // multi-device layout: whole card proceeds
    bool* revealSerial = nullptr;
};
DeviceCardAction DeviceCard(const DeviceInfo& info, float width, const DeviceCardOptions& opts);

// ---- Data display ----------------------------------------------------------

// Byte formatting honouring Settings > Units (set once per frame by App).
void SetBinaryUnits(bool binary);
std::string Bytes(uint64_t bytes);
std::string Count(uint64_t n); // "41,851"
std::string Date(int64_t unixSeconds); // "2026-08-12" local time

ImVec4 CategoryColor(uint8_t category);    // ps::Category as int
const char* CategoryIcon(uint8_t category);

struct DonutSegment {
    double value;
    ImVec4 color;
};
// Ring chart drawn at the cursor; `fill` (0..1) animates the sweep in.
void Donut(const DonutSegment* segs, int count, float radius, float thickness, std::string_view centerTop,
    std::string_view centerBottom, float fill = 1.0f);

// Pulsing placeholder rows while a list loads.
void SkeletonRows(int rows, float rowHeight);

// Inline error card: plain-English message, [Retry], and a "details" expander.
// Returns true when Retry is clicked.
bool ErrorCard(const char* id, std::string_view message, std::string_view details, const char* retryLabel = "Retry");

// Small titled panel used on the Overview dashboard.
void BeginPanel(const char* id, const char* title, const ImVec2& size);
void EndPanel();

// ---- App logo ----------------------------------------------------------------

void SetLogoTexture(uint64_t texture);
// Draws the logo (rounded) at the cursor, `size` px square. Falls back to the phone glyph.
void Logo(float size);
// Small accent dot on a button corner (v1.1: update available).
void UpdateDot(ImVec2 itemMin, ImVec2 itemMax);

// ---- Toasts (bottom-right, 3 s) --------------------------------------------

enum class ToastKind { Info, Success, Warning, Error };
void Toast(std::string text, ToastKind kind = ToastKind::Success);
void DrawToasts(float bottomMargin);

} // namespace ps::ui
