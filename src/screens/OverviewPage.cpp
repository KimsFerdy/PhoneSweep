#include "screens/OverviewPage.h"

#include "app/AppContext.h"
#include "scan/AppSession.h"
#include "scan/FileStore.h"
#include "report/Exporter.h"
#include "screens/AppsPage.h"
#include "scan/ScanSession.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace ps {

using namespace ps::theme;

void OverviewPage::Draw(AppContext& ctx)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = S(16);
    const float leftW = std::floor((avail.x - gap) * 0.6f);
    const float rightW = avail.x - gap - leftW;
    // The storage panel measures its own content each frame; never clip it.
    const float topH = std::max({ S(290), avail.y * 0.48f, m_storageNeed });
    // Leave room for the trailing item spacing so the page never needs a scrollbar.
    const float bottomH = std::max(S(220), avail.y - topH - gap - ImGui::GetStyle().ItemSpacing.y);

    // Scan failed with nothing to show: the error card is the dashboard.
    if (ctx.files.Files().empty() && ctx.scan.GetState() == ScanSession::State::Failed) {
        ImGui::Dummy(ImVec2(0, S(24)));
        const auto o = ctx.scan.Outcome();
        if (ui::ErrorCard("scanerr", o.error, o.errorDetail, ICON_ROTATE "  Retry scan"))
            ctx.requestScan = true;
        return;
    }

    DrawStorage(ctx, leftW, topH);
    ImGui::SameLine(0, gap);
    DrawQuickActions(ctx, rightW, topH);
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    DrawBiggest(ctx, leftW, bottomH);
    ImGui::SameLine(0, gap);
    DrawAttention(ctx, rightW, bottomH);
}

void OverviewPage::DrawStorage(AppContext& ctx, float w, float h)
{
    const FileStore& store = ctx.files;
    ui::BeginPanel("##storage", ICON_HARD_DRIVE "  Storage", ImVec2(w, h));

    ui::DonutSegment segs[kCategoryCount];
    for (size_t c = 0; c < kCategoryCount; ++c)
        segs[c] = { static_cast<double>(store.ByCategory(static_cast<Category>(c)).bytes), ui::CategoryColor(static_cast<uint8_t>(c)) };

    const float radius = std::min(S(92), (h - S(70)) * 0.5f);
    const bool scanning = ctx.scan.Running();
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, S(6)));
    ui::Donut(segs, static_cast<int>(kCategoryCount), radius, S(18), ui::Bytes(store.All().bytes),
        scanning ? std::format("{} files\xE2\x80\xA6", ui::Count(store.All().count)) : std::format("in {} files", ui::Count(store.All().count)));
    ImGui::EndGroup();

    ImGui::SameLine(0, S(28));
    ImGui::BeginGroup();
    const float legendW = ImGui::GetContentRegionAvail().x;
    const float groupX = ImGui::GetCursorPosX(); // window-local; SameLine(x) would be group-relative
    auto rightAlign = [&](std::string_view text) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(groupX + legendW - ui::TextWidth(text));
    };
    auto legendRow = [&](const ImVec4& col, const char* label, uint64_t bytes, bool dot = true) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (dot)
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + S(4), p.y + ImGui::GetTextLineHeight() * 0.5f), S(4), U32(col));
        ImGui::SetCursorPosX(groupX + S(16));
        if (dot)
            ImGui::TextUnformatted(label);
        else
            ui::DimText(label);
        const std::string b = ui::Bytes(bytes);
        rightAlign(b);
        ui::DimText(b);
    };
    // Biggest categories first.
    std::array<size_t, kCategoryCount> order{};
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::ranges::sort(order, [&](size_t a, size_t b) {
        return store.ByCategory(static_cast<Category>(a)).bytes > store.ByCategory(static_cast<Category>(b)).bytes;
    });
    for (size_t c : order)
        legendRow(ui::CategoryColor(static_cast<uint8_t>(c)), CategoryName(static_cast<Category>(c)),
            store.ByCategory(static_cast<Category>(c)).bytes);

    // Messaging media is usually what fills a phone: call it out.
    const uint64_t wa = store.BySource(Source::WhatsApp).bytes + store.BySource(Source::WhatsAppBusiness).bytes;
    const uint64_t tg = store.BySource(Source::Telegram).bytes;
    if (wa || tg) {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (wa)
            legendRow(Success, "  WhatsApp media", wa, false);
        if (tg)
            legendRow(Success, "  Telegram media", tg, false);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    ui::HLine(0.7f);
    ImGui::Dummy(ImVec2(0, S(4)));

    // Per-volume capacity bars.
    {
        Font f(GetFonts().ui, FontSmall);
        for (const auto& v : store.Volumes()) {
            if (!v.totalBytes)
                continue;
            const double frac = static_cast<double>(v.usedBytes) / static_cast<double>(v.totalBytes);
            ImGui::TextUnformatted(v.label.c_str());
            const std::string right = std::format("{} free of {}", ui::Bytes(v.freeBytes), ui::Bytes(v.totalBytes));
            rightAlign(right);
            ui::ColoredText(frac > 0.9 ? Danger : TextDim, right);
            ui::StorageBar(frac, legendW, S(6));
            ImGui::Dummy(ImVec2(0, S(2)));
        }
        if (store.Volumes().empty() && ctx.activeInfo.storage) {
            const auto& s = *ctx.activeInfo.storage;
            ImGui::TextUnformatted("Internal");
            ui::StorageBar(s.UsedFraction(), legendW, S(6));
        }
    }
    ImGui::EndGroup();
    m_storageNeed = ImGui::GetCursorPosY() + S(14); // content height incl. padding, used next frame
    ui::EndPanel();
}

void OverviewPage::DrawQuickActions(AppContext& ctx, float w, float h)
{
    ui::BeginPanel("##actions", ICON_BOLT "  Quick actions", ImVec2(w, h));
    const float bw = ImGui::GetContentRegionAvail().x;
    const bool connected = ctx.ActiveConnected();
    const bool scanning = ctx.scan.Running();

    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (scanning) {
        if (ui::SecondaryButton(ICON_XMARK "  Cancel scan", ImVec2(bw, 0)))
            ctx.scan.Cancel();
    } else if (ui::SecondaryButton(ICON_ROTATE "  Rescan", ImVec2(bw, 0), connected)) {
        ctx.requestScan = true;
        ctx.requestAppScan = true;
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    if (ui::SecondaryButton(ICON_BROOM "  Clear all app caches", ImVec2(bw, 0), connected))
        m_confirmTrim = true;
    {
        Font f(GetFonts().ui, FontSmall);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
        ui::DimText("Asks Android to drop cached data (pm trim-caches). No personal files.");
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    if (ui::SecondaryButton(ICON_FILES "  Find duplicates & junk", ImVec2(bw, 0), !ctx.files.Files().empty()))
        ctx.page = Page::Cleanup;
    ImGui::Dummy(ImVec2(0, S(2)));
    if (ui::SecondaryButton(ICON_FILE_EXPORT "  Export report", ImVec2(bw, 0), !ctx.files.Files().empty() || !ctx.apps.Apps().empty()))
        ctx.requestExport = Export_All;
    ImGui::SetItemTooltip("Writes applist.txt + scan_report.md + the session log to the reports folder.");
    ImGui::Dummy(ImVec2(0, S(2)));
    {
        const Totals& kept = ctx.files.Kept();
        const std::string label = kept.count ? std::format(ICON_FLOPPY "  Save everything important ({} \xC2\xB7 {})", ui::Count(kept.count), ui::Bytes(kept.bytes))
                                             : std::string(ICON_FLOPPY "  Save everything important\xE2\x80\xA6");
        if (ui::SecondaryButton(label.c_str(), ImVec2(bw, 0), kept.count > 0 && connected)) {
            ctx.requestSaveKept = true;
            ctx.page = Page::SaveQueue;
        }
        ImGui::SetItemTooltip(kept.count ? "Saves every file marked KEEP that isn't verified on the PC yet."
                                         : "Mark files as KEEP in Files first (select them, then \"Mark as keep\").");
    }
    ImGui::PopStyleVar();

    // Confirmation for cache clearing.
    if (m_confirmTrim) {
        ImGui::OpenPopup("Clear app caches?");
        m_confirmTrim = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Clear app caches?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("Android will drop every app's cached data to free space. Apps rebuild caches as needed; "
                           "your photos, messages and settings are not touched.");
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ui::PrimaryButton(ICON_BROOM "  Clear caches")) {
            ctx.requestTrimCaches = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ui::EndPanel();
}

void OverviewPage::DrawBiggest(AppContext& ctx, float w, float h)
{
    const FileStore& store = ctx.files;
    if (store.Version() != m_biggestVersion) {
        m_biggest.clear();
        const auto& f = store.Files();
        for (uint32_t i = 0; i < f.size(); ++i)
            if (!f[i].IsProtected())
                m_biggest.push_back(i);
        const size_t n = std::min<size_t>(10, m_biggest.size());
        std::partial_sort(m_biggest.begin(), m_biggest.begin() + n, m_biggest.end(),
            [&](uint32_t a, uint32_t b) { return f[a].size > f[b].size; });
        m_biggest.resize(n);
        m_biggestVersion = store.Version();
    }

    ui::BeginPanel("##biggest", ICON_FILE "  Biggest files", ImVec2(w, h));
    if (m_biggest.empty()) {
        if (ctx.scan.Running())
            ui::SkeletonRows(6, S(30));
        else
            ui::EmptyState(ICON_FILES, "Nothing scanned yet");
    }
    const float rowH = S(30);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const size_t fits = static_cast<size_t>(std::max(1.0f, ImGui::GetContentRegionAvail().y / rowH));
    for (size_t k = 0; k < std::min(fits, m_biggest.size()); ++k) {
        const uint32_t fi = m_biggest[k];
        const FileEntry& e = store.Files()[fi];
        ImGui::PushID(static_cast<int>(fi));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rw = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("##big", ImVec2(rw, rowH))) {
            ctx.focusFile = fi;
            ctx.page = Page::Files;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rowH), U32(PanelHover, 0.7f), S(RoundButton));
            ImGui::SetTooltip("%s\nClick to open in Files", e.path.c_str());
        }
        const float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(p.x + S(8), ty), U32(ui::CategoryColor(static_cast<uint8_t>(e.category))),
            ui::CategoryIcon(static_cast<uint8_t>(e.category)));
        const std::string size = ui::Bytes(e.size);
        const char* src = e.source == Source::Other ? "" : SourceName(e.source);
        const float rightW = S(220);
        dl->PushClipRect(ImVec2(p.x + S(34), p.y), ImVec2(p.x + rw - rightW, p.y + rowH), true);
        dl->AddText(ImVec2(p.x + S(34), ty), U32(Text), e.Name().data(), e.Name().data() + e.Name().size());
        dl->PopClipRect();
        dl->AddText(ImVec2(p.x + rw - rightW + S(10), ty), U32(Text), size.c_str());
        dl->AddText(ImVec2(p.x + rw - rightW + S(100), ty), U32(TextDim), src);
        ImGui::PopID();
    }
    ui::EndPanel();
}

void OverviewPage::DrawAttention(AppContext& ctx, float w, float h)
{
    const FileStore& store = ctx.files;
    ui::BeginPanel("##attention", ICON_BELL "  Attention", ImVec2(w, h));

    int shown = 0;
    auto item = [&](const char* icon, const ImVec4& col, const std::string& text) {
        ui::ColoredText(col, icon);
        ImGui::SameLine(0, S(10));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(2)));
        ++shown;
    };

    for (const auto& v : store.Volumes()) {
        if (v.totalBytes && static_cast<double>(v.usedBytes) / static_cast<double>(v.totalBytes) > 0.9)
            item(ICON_WARNING, Danger, std::format("{} storage is {:.0f}% full ({} free)", v.label,
                                           100.0 * static_cast<double>(v.usedBytes) / static_cast<double>(v.totalBytes), ui::Bytes(v.freeBytes)));
    }
    // Apps first after storage: most important, and it must stay visible at 150%.
    // (Clickable: opens Apps with the Flagged filter.)
    if (const size_t flagged = ctx.apps.FlaggedCount(); flagged) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const size_t high = ctx.apps.HighRiskCount();
        item(ICON_WARNING, high ? Danger : Warning,
            high ? std::format("{} apps with high-risk access ({} flagged in total)  " ICON_CHEVRON_RIGHT, high, flagged)
                 : std::format("{} apps flagged  " ICON_CHEVRON_RIGHT, flagged));
        if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y))) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                AppsPage::RequestFlaggedFilter();
                ctx.page = Page::Apps;
            }
        }
    }
    if (const size_t boot = ctx.apps.BootCount(); boot)
        item(ICON_POWER, Info, std::format("{} apps start at boot", boot));
    if (ctx.apps.Running()) {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText(ICON_CUBES "  Scanning apps\xE2\x80\xA6");
    }

    const Totals wa{ store.BySource(Source::WhatsApp).count + store.BySource(Source::WhatsAppBusiness).count,
        store.BySource(Source::WhatsApp).bytes + store.BySource(Source::WhatsAppBusiness).bytes };
    if (wa.count)
        item(ICON_INFO, Info, std::format("{} WhatsApp media files \xC2\xB7 {}", ui::Count(wa.count), ui::Bytes(wa.bytes)));
    if (const auto& tg = store.BySource(Source::Telegram); tg.count)
        item(ICON_INFO, Info, std::format("{} Telegram media files \xC2\xB7 {}", ui::Count(tg.count), ui::Bytes(tg.bytes)));
    if (const auto& ar = store.ByCategory(Category::Archives); ar.count)
        item(ICON_INFO, Info, std::format("{} APKs/archives \xC2\xB7 {} (old installers are often safe to drop)", ui::Count(ar.count),
                                  ui::Bytes(ar.bytes)));
    if (store.Protected().count)
        item(ICON_LOCK, TextDim, std::format("{} files in app-data / hidden folders \xC2\xB7 {} (left alone)",
                                     ui::Count(store.Protected().count), ui::Bytes(store.Protected().bytes)));

    const auto o = ctx.scan.Outcome();
    if (o.permissionDenied)
        item(ICON_WARNING, Warning, std::format("{} folders couldn't be read (permission denied)", ui::Count(o.permissionDenied)));
    if (o.unreadableNames)
        item(ICON_WARNING, Warning, std::format("{} files have broken names the phone can't open", ui::Count(o.unreadableNames)));

    if (shown == 0 && !ctx.scan.Running() && store.Files().empty())
        ui::DimText("Run a scan to see what needs attention.");
    ui::EndPanel();
}

} // namespace ps
