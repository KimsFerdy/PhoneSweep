#include "screens/WorkspaceScreen.h"

#include "app/Version.h"

#include "platform/Platform.h"
#include "scan/ScanSession.h"
#include "scan/AppSession.h"
#include "screens/AppsPage.h"
#include "screens/BackgroundPage.h"
#include "screens/CleanupPage.h"
#include "report/Exporter.h"
#include "screens/NukePage.h"
#include "screens/ReportsPage.h"
#include "screens/BackupPage.h"
#include "transfer/Puller.h"
#include "screens/FilesPage.h"
#include "screens/OverviewPage.h"
#include "ui/Icons.h"
#include "util/JobSystem.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Strings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

constexpr const char* kContentWindow = "###Content";

struct NavItem {
    Page page;
    const char* icon;
    const char* label;
    const char* futureVersion; // non-null = greyed out, "coming in vX"
};

constexpr NavItem kNav[] = {
    { Page::Overview, ICON_GAUGE, "Overview", nullptr },
    { Page::Files, ICON_FILES, "Files", nullptr },
    { Page::Cleanup, ICON_BROOM, "Cleanup", nullptr },
    { Page::Apps, ICON_CUBES, "Apps", nullptr },
    { Page::Background, ICON_POWER, "Background & Startup", nullptr },
    { Page::SaveQueue, ICON_DOWNLOAD, "Backup", nullptr },
    { Page::Nuke, ICON_FIRE, "Nuke", nullptr },
    { Page::Reports, ICON_FILE_EXPORT, "Reports", nullptr },
};

// Stand-in for screens that arrive in later milestones.
class PlaceholderPage final : public IScreen {
public:
    PlaceholderPage(const char* icon, const char* title, const char* when)
        : m_icon(icon), m_title(title), m_when(when) {}
    void Draw(AppContext&) override { ui::EmptyState(m_icon, m_title, m_when); }

private:
    const char* m_icon;
    const char* m_title;
    const char* m_when;
};

} // namespace

WorkspaceScreen::WorkspaceScreen()
{
    auto set = [&](Page p, const char* icon, const char* title, const char* when) {
        m_pages[static_cast<size_t>(p)] = std::make_unique<PlaceholderPage>(icon, title, when);
    };
    m_pages[static_cast<size_t>(Page::Overview)] = std::make_unique<OverviewPage>();
    m_pages[static_cast<size_t>(Page::Files)] = std::make_unique<FilesPage>();
    m_pages[static_cast<size_t>(Page::Cleanup)] = std::make_unique<CleanupPage>();
    m_pages[static_cast<size_t>(Page::Apps)] = std::make_unique<AppsPage>();
    m_pages[static_cast<size_t>(Page::Background)] = std::make_unique<BackgroundPage>();
    m_pages[static_cast<size_t>(Page::SaveQueue)] = std::make_unique<BackupPage>();
    m_pages[static_cast<size_t>(Page::Nuke)] = std::make_unique<NukePage>();
    m_pages[static_cast<size_t>(Page::Reports)] = std::make_unique<ReportsPage>();
}

WorkspaceScreen::~WorkspaceScreen() = default;

void WorkspaceScreen::Draw(AppContext& ctx)
{
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L))
        ctx.showLog = !ctx.showLog;

    // Keep the latest device info while connected; keep the stale copy otherwise.
    if (const DeviceInfo* info = ctx.devices.FindInfo(ctx.activeSerial))
        ctx.activeInfo = *info;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Begin("##Workspace", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
    ImGui::PopStyleVar(4);
    // App chrome (top bar, sidebar, status bar) is edge-to-edge: no rounded child corners.
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

    DrawTopBar(ctx);
    if (!ctx.ActiveConnected())
        DrawDisconnectBanner(ctx);
    if (ctx.scan.Running())
        DrawScanStrip(ctx);

    const float midH = ImGui::GetContentRegionAvail().y - S(StatusBarHeight);
    DrawSidebar(ctx, midH);
    ImGui::SameLine(0, 0);
    DrawDockArea(ctx, ImGui::GetContentRegionAvail().x, midH);
    DrawStatusBar(ctx);

    ImGui::PopStyleVar(2);
    ImGui::End();

    // Docked windows are submitted outside the host window.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    ImGui::Begin(kContentWindow, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    DrawPage(ctx);
    ImGui::End();

    if (ctx.showLog)
        m_log.Draw(ctx);
}

void WorkspaceScreen::DrawTopBar(AppContext& ctx)
{
    const float h = S(TopBarHeight);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(8)));
    ImGui::BeginChild("##topbar", ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);

    const float midY = h * 0.5f;
    auto centerFrame = [&] { ImGui::SetCursorPosY(midY - ImGui::GetFrameHeight() * 0.5f); };
    // Everything after the first button shares its line, and ImGui already adds
    // the frame-padding baseline offset to text there, so text aligns like a frame.
    auto centerText = centerFrame;
    auto divider = [&] {
        ImGui::SameLine(0, S(12));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float wy = ImGui::GetWindowPos().y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, wy + h * 0.28f), ImVec2(p.x, wy + h * 0.72f), U32(Border));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::SameLine(0, S(12));
    };

    centerFrame();
    if (ui::GhostButton(ICON_ARROW_LEFT "  Devices"))
        ctx.Navigate(Route::Home);
    divider();

    const DeviceInfo& d = ctx.activeInfo;
    centerText();
    ui::ColoredText(ctx.ActiveConnected() ? Accent : TextDim, ICON_PHONE);
    ImGui::SameLine(0, S(8));
    centerText();
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(d.displayName.c_str());
    }
    ImGui::SameLine(0, S(6));
    centerText();
    ui::DimText(std::format("\xC2\xB7  Android {}", d.android));
    ImGui::SameLine(0, S(8));
    centerText();
    ui::DimText(d.wireless ? ICON_WIFI : ICON_PLUG);
    ImGui::SetItemTooltip(d.wireless ? "Connected over Wi-Fi" : "Connected over USB");

    if (d.storage && d.storage->total) {
        divider();
        const double f = d.storage->UsedFraction();
        ImGui::SetCursorPosY(midY - S(4));
        ui::StorageBar(f, S(90), S(8));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s used of %s, %s free", FormatBytes(d.storage->used).c_str(),
                FormatBytes(d.storage->total).c_str(), FormatBytes(d.storage->free).c_str());
        ImGui::SameLine(0, S(8));
        centerText();
        ui::ColoredText(f >= 0.75 ? ui::StorageColor(f) : Text, std::format("{:.0f}%", f * 100.0));
    }
    if (d.batteryLevel) {
        divider();
        centerText();
        ui::ColoredText(*d.batteryLevel <= 15 ? Danger : Text,
            std::format("{}{} {}%", d.charging ? ICON_BOLT " " : "", ICON_BATTERY_3Q, *d.batteryLevel));
    }

    // Right side: settings + log toggle.
    const char* logLabel = ICON_BARS "  Log";
    const float logW = ui::TextWidth(logLabel) + ImGui::GetStyle().FramePadding.x * 2;
    const float gearW = ui::TextWidth(ICON_GEAR) + ImGui::GetStyle().FramePadding.x * 2;
    const float wifiW = ui::TextWidth(ICON_WIFI) + ImGui::GetStyle().FramePadding.x * 2 + S(6);
    const char* exportLabel = ICON_FILE_EXPORT "  Export";
    const float exportW = ui::TextWidth(exportLabel) + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - logW - gearW - wifiW - exportW - S(12));
    centerFrame();
    // Export menu (build spec M6).
    if (ui::GhostButton(exportLabel))
        ImGui::OpenPopup("##exportmenu");
    if (ImGui::BeginPopup("##exportmenu")) {
        if (ImGui::MenuItem(ICON_FILE_EXPORT "  Export report"))
            ctx.requestExport = Export_All;
        if (ImGui::MenuItem(ICON_FILE_LINES "  Export scan report only"))
            ctx.requestExport = Export_ScanReport;
        if (ImGui::MenuItem(ICON_COPY "  Copy app list to clipboard"))
            ctx.requestCopyAppList = true;
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FOLDER_OPEN "  Reports\xE2\x80\xA6"))
            ctx.page = Page::Reports;
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(6));
    centerFrame();
    if (ui::GhostButton(ICON_WIFI "##wireless"))
        ctx.showWireless = true;
    ImGui::SetItemTooltip("Connect over Wi-Fi");
    ImGui::SameLine(0, S(6));
    centerFrame();
    if (ui::GhostButton(ICON_GEAR "##settings"))
        ctx.showSettings = true;
    ImGui::SetItemTooltip("Settings");
    ImGui::SameLine(0, S(6));
    centerFrame();
    if (ctx.showLog)
        ImGui::PushStyleColor(ImGuiCol_Button, Hex(0x3DD6C6, 0.14f));
    if (ui::GhostButton(logLabel))
        ctx.showLog = !ctx.showLog;
    if (ctx.showLog)
        ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Toggle log (Ctrl+L)");

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void WorkspaceScreen::DrawDisconnectBanner(AppContext&)
{
    const float h = S(34);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(Warning, 0.14f));
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + w, p.y + h - 1), U32(Warning, 0.4f));
    const char* msg = ICON_LINK_SLASH "  Phone disconnected: jobs paused. Reconnect to continue.";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + S(16), p.y + (h - ts.y) * 0.5f), U32(Warning), msg);
    ImGui::Dummy(ImVec2(w, h));
}

void WorkspaceScreen::DrawScanStrip(AppContext& ctx)
{
    // Thin progress line under the top bar: "Scanning… 18,402 files".
    const float h = S(24);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(Panel));

    // Indeterminate bar: a short accent segment sweeping across the bottom edge.
    const float t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1.0));
    const float segW = w * 0.22f;
    const float x0 = p.x - segW + (w + segW) * t;
    dl->AddRectFilled(ImVec2(p.x, p.y + h - S(2)), ImVec2(p.x + w, p.y + h), U32(Border));
    dl->AddRectFilled(ImVec2(std::max(p.x, x0), p.y + h - S(2)), ImVec2(std::min(p.x + w, x0 + segW), p.y + h), U32(Accent));

    Font f(GetFonts().ui, FontSmall);
    const std::string text = std::format("Scanning\xE2\x80\xA6 {} files  \xC2\xB7  {}", ui::Count(ctx.scan.FilesFound()), ctx.scan.Phase());
    dl->AddText(ImVec2(p.x + S(16), p.y + (h - S(2) - ImGui::GetTextLineHeight()) * 0.5f), U32(TextDim), text.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

void WorkspaceScreen::DrawSidebar(AppContext& ctx, float height)
{
    const bool collapsed = ImGui::GetMainViewport()->WorkSize.x < S(SidebarCollapseBelow);
    const float w = S(collapsed ? SidebarCollapsedWidth : SidebarWidth);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
    ImGui::BeginChild("##sidebar", ImVec2(w, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float itemH = S(36);
    for (const auto& item : kNav) {
        if (item.futureVersion) {
            ImGui::Dummy(ImVec2(0, S(8)));
            ui::HLine(0.8f);
            ImGui::Dummy(ImVec2(0, S(8)));
        }
        const bool active = ctx.page == item.page;
        const bool future = item.futureVersion != nullptr;
        const bool danger = item.page == Page::Nuke;

        ImGui::PushID(static_cast<int>(item.page));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float iw = ImGui::GetContentRegionAvail().x;
        const bool clicked = ImGui::InvisibleButton("##nav", ImVec2(iw, itemH)) && !future;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && !future)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked)
            ctx.page = item.page;

        if (active || (hovered && !future))
            dl->AddRectFilled(p, ImVec2(p.x + iw, p.y + itemH), U32(active ? PanelRaised : PanelHover, active ? 1.0f : 0.6f), S(RoundButton));
        if (active)
            dl->AddRectFilled(ImVec2(p.x, p.y + S(8)), ImVec2(p.x + S(3), p.y + itemH - S(8)), U32(Accent), S(2));

        const ImVec4 col = future ? Mix(TextDim, Panel, 0.45f) : danger ? Danger : active ? Text : TextDim;
        const float ty = p.y + (itemH - ImGui::GetTextLineHeight()) * 0.5f;
        const float iconX = collapsed ? p.x + (iw - S(FontBase * 1.25f)) * 0.5f : p.x + S(12);
        dl->AddText(ImVec2(iconX, ty), U32(danger ? Danger : active ? Accent : col), item.icon);
        if (!collapsed) {
            dl->AddText(ImVec2(p.x + S(40), ty), U32(col), item.label);
            if (future) {
                Font f(GetFonts().ui, FontSmall - 1);
                const ImVec2 ts = ImGui::CalcTextSize(item.futureVersion);
                dl->AddText(ImVec2(p.x + iw - ts.x - S(10), p.y + (itemH - ts.y) * 0.5f), U32(col), item.futureVersion);
            }
            // Badge: high-risk apps on Apps (UI spec: "Apps 3⚠").
            if (item.page == Page::Apps) {
                if (const size_t high = ctx.apps.HighRiskCount(); high) {
                    Font f(GetFonts().uiStrong, FontSmall - 1);
                    const std::string badge = std::format("{} " ICON_WARNING, high);
                    const ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
                    const ImVec2 bmax(p.x + iw - S(8), p.y + (itemH + ts.y) * 0.5f + S(2));
                    const ImVec2 bmin(bmax.x - ts.x - S(12), bmax.y - ts.y - S(4));
                    dl->AddRectFilled(bmin, bmax, U32(Warning, 0.16f), (bmax.y - bmin.y) * 0.5f);
                    dl->AddText(ImVec2(bmin.x + S(6), bmin.y + S(2)), U32(Warning), badge.c_str());
                }
            }
        }
        if (hovered && (collapsed || future))
            ImGui::SetTooltip("%s%s", item.label, future ? std::format(" (coming in {})", item.futureVersion).c_str() : "");
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void WorkspaceScreen::BuildDockLayout(unsigned dockId, float width, float height)
{
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImVec2(width, height));
    ImGuiID center = dockId;
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGui::DockBuilderDockWindow(kContentWindow, center);
    ImGui::DockBuilderDockWindow(LogPanel::kWindowName, bottom);
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(center))
        node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe | ImGuiDockNodeFlags_NoUndocking;
    // The Log tab already has its own close button; drop the node's duplicate.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(bottom))
        node->LocalFlags |= ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoWindowMenuButton;
    ImGui::DockBuilderFinish(dockId);
}

void WorkspaceScreen::DrawDockArea(AppContext& ctx, float width, float height)
{
    const ImGuiID dockId = ImGui::GetID("WorkspaceDock");

    // Re-dock the log at the bottom when it is reopened and its node was removed
    // (ImGui collapses an empty split when its last window closes).
    bool rebuild = !m_layoutBuilt || !ImGui::DockBuilderGetNode(dockId);
    if (ctx.showLog && !m_logWasShown) {
        ImGuiWindow* logWin = ImGui::FindWindowByName(LogPanel::kWindowName);
        if (!logWin || logWin->DockId == 0 || !ImGui::DockBuilderGetNode(logWin->DockId))
            rebuild = true;
    }
    m_logWasShown = ctx.showLog;
    if (rebuild) {
        BuildDockLayout(dockId, width, height);
        m_layoutBuilt = true;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, Background);
    ImGui::DockSpace(dockId, ImVec2(width, height), ImGuiDockNodeFlags_None);
    ImGui::PopStyleColor();
}

void WorkspaceScreen::DrawStatusBar(AppContext& ctx)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), 0));
    ImGui::BeginChild("##status", ImVec2(0, S(StatusBarHeight)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    {
    Font f(GetFonts().ui, FontSmall);
    ImGui::SetCursorPosY((S(StatusBarHeight) - ImGui::GetTextLineHeight()) * 0.5f);

    if (ctx.devices.adbFound)
        ui::ColoredText(Success, "adb " ICON_CHECK);
    else
        ui::ColoredText(Danger, "adb " ICON_XMARK);
    ImGui::SameLine(0, S(12));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float th = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + th * 0.15f), ImVec2(p.x, p.y + th * 0.85f), U32(Border));
        ImGui::Dummy(ImVec2(1, th));
    }
    ImGui::SameLine(0, S(12));
    std::vector<JobPtr> jobs = ctx.jobs.Visible();
    // Save workers are shown as one line with queue totals, not as separate jobs.
    std::erase_if(jobs, [](const JobPtr& j) { return j->Name() == "Saving to PC"; });
    const auto save = ctx.saveQueue.GetStats();
    if (!ctx.ActiveConnected()) {
        ui::ColoredText(Warning, save.queued + save.active ? "Paused: phone disconnected (save queue waiting)" : "Paused: phone disconnected");
    } else if (save.status == SaveQueue::Status::Running) {
        const float sp = S(6);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - sp * 2) * 0.5f);
        ui::Spinner(sp, S(1.5f), Accent);
        ImGui::SameLine(0, S(8));
        ImGui::SetCursorPosY((S(StatusBarHeight) - ImGui::GetTextLineHeight()) * 0.5f);
        const int eta = save.etaSeconds < 0 ? -1 : static_cast<int>(save.etaSeconds);
        ImGui::TextUnformatted(std::format("Saving {}/{} \xC2\xB7 {}/s{}", save.done + save.skipped, save.total,
            ui::Bytes(static_cast<uint64_t>(save.bytesPerSec)),
            eta < 0 ? std::string() : eta < 60 ? std::format(" \xC2\xB7 ETA {}s", eta) : std::format(" \xC2\xB7 ETA {}m", (eta + 59) / 60))
                                   .c_str());
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY((S(StatusBarHeight) - S(6)) * 0.5f);
        ui::ProgressBarThin(save.bytesTotal ? static_cast<float>(save.bytesDone) / static_cast<float>(save.bytesTotal) : 0.0f, S(120), S(6), Accent);
        ImGui::SameLine(0, S(10));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), 0));
        ImGui::SetCursorPosY((S(StatusBarHeight) - ImGui::GetTextLineHeight()) * 0.5f);
        if (ui::GhostButton("Cancel##save"))
            ctx.saveQueue.CancelAll();
        ImGui::PopStyleVar();
    } else if (!jobs.empty()) {
        const JobPtr& job = jobs.front();
        const float sp = S(6);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - sp * 2) * 0.5f);
        ui::Spinner(sp, S(1.5f), Accent);
        ImGui::SameLine(0, S(8));
        ImGui::SetCursorPosY((S(StatusBarHeight) - ImGui::GetTextLineHeight()) * 0.5f);
        const std::string detail = job->Detail();
        ImGui::TextUnformatted(job->Name().c_str());
        if (!detail.empty()) {
            ImGui::SameLine(0, S(6));
            ui::DimText("\xC2\xB7 " + detail);
        }
        if (jobs.size() > 1) {
            ImGui::SameLine(0, S(6));
            ui::DimText(std::format("(+{} queued)", jobs.size() - 1));
        }
        ImGui::SameLine(0, S(12));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), 0));
        ImGui::SetCursorPosY((S(StatusBarHeight) - ImGui::GetTextLineHeight()) * 0.5f);
        if (ui::GhostButton("Cancel##job"))
            job->Cancel();
        ImGui::PopStyleVar();
    } else {
        ui::DimText("Idle");
    }

    const char* ver = kAppNameVersion;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ui::TextWidth(ver));
    ui::DimText(ver);

    // The whole bar opens the save queue (UI spec §4).
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        ctx.page = Page::SaveQueue;
    } // font scope must close before EndChild
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void WorkspaceScreen::DrawPage(AppContext& ctx)
{
    if (auto& page = m_pages[static_cast<size_t>(ctx.page)])
        page->Draw(ctx);
}

} // namespace ps
