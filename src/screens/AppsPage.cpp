#include "screens/AppsPage.h"

#include "app/AppContext.h"
#include "nuke/NukePlan.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
#include "platform/Platform.h"
#include "report/AppListFormat.h"
#include "scan/AppSession.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool IContains(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
        return true;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && Lower(hay[i + j]) == Lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

ImVec4 SeverityColor(FlagSeverity s)
{
    switch (s) {
    case FlagSeverity::High: return Danger;
    case FlagSeverity::Medium: return Warning;
    default: return Info;
    }
}

// Letter avatar: first letter of the label, or of the package's last segment.
void Avatar(ImDrawList* dl, ImVec2 p, float size, const AppInfo& a)
{
    std::string_view src = a.label;
    if (src.empty()) {
        src = a.package;
        if (const auto dot = src.rfind('.'); dot != std::string_view::npos && dot + 1 < src.size())
            src = src.substr(dot + 1);
    }
    char letter = src.empty() ? '?' : src.front();
    if (letter >= 'a' && letter <= 'z')
        letter = static_cast<char>(letter - 32);
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= '0' && letter <= '9')))
        letter = '?';
    uint32_t h = 2166136261u;
    for (char c : a.package)
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    static constexpr ImVec4 kColors[] = { CatImages, CatVideos, CatMusic, CatDocs, CatArchives, CatApps };
    const ImVec4 col = a.risk >= 25 ? Danger : kColors[h % std::size(kColors)];
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), U32(col, 0.16f), size * 0.28f);
    Font f(GetFonts().uiStrong, size / Dpi() * 0.45f);
    const char str[2] = { letter, 0 };
    const ImVec2 ts = ImGui::CalcTextSize(str);
    dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), U32(col), str);
}

// Pill width as ui::Pill draws it (bold small text + padding).
float PillWidth(const char* text)
{
    Font f(GetFonts().uiStrong, FontSmall - 1.0f);
    return ImGui::CalcTextSize(text).x + S(7) * 2;
}

// Draws pills that fit in maxWidth; the rest collapse into "+N" with a tooltip.
void FlagPills(const AppInfo& a, bool includeInfo, float maxWidth)
{
    std::vector<const FlagInfo*> list;
    for (const auto& fi : AllFlags())
        if ((a.flags & fi.flag) && (includeInfo || fi.severity != FlagSeverity::Info))
            list.push_back(&fi);
    const float gap = S(4);
    const float moreW = PillWidth("+9");
    float used = 0;
    size_t shown = 0;
    for (; shown < list.size(); ++shown) {
        const float w = PillWidth(list[shown]->label);
        const bool last = shown + 1 == list.size();
        if (used + w + (last ? 0 : gap + moreW) > maxWidth && shown > 0)
            break;
        if (shown > 0)
            ImGui::SameLine(0, gap);
        ui::Pill(list[shown]->label, SeverityColor(list[shown]->severity));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", list[shown]->why);
        used += w + gap;
    }
    if (shown < list.size()) {
        ImGui::SameLine(0, gap);
        ui::Pill(std::format("+{}", list.size() - shown), TextDim);
        if (ImGui::IsItemHovered()) {
            std::string tip;
            for (size_t i = shown; i < list.size(); ++i)
                tip += std::format("{}: {}\n", list[i]->label, list[i]->why);
            tip.pop_back();
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
}

std::string Size(const AppInfo& a) { return a.HasSize() ? ui::Bytes(a.TotalBytes()) : std::string("\xE2\x80\x94"); }

} // namespace

const AppInfo* AppsPage::FindApp(const AppSession& apps, std::string_view pkg) const
{
    for (const auto& a : apps.Apps())
        if (a.package == pkg)
            return &a;
    return nullptr;
}

std::vector<const AppInfo*> AppsPage::SelectedApps(const AppSession& apps) const
{
    std::vector<const AppInfo*> out;
    for (const auto& a : apps.Apps())
        if (m_selected.contains(a.package))
            out.push_back(&a);
    return out;
}

void AppsPage::RebuildView(const AppSession& session)
{
    const auto& apps = session.Apps();
    const std::string_view search = m_search;
    m_view.clear();
    for (int i = 0; i < static_cast<int>(apps.size()); ++i) {
        const auto& a = apps[i];
        if (a.system != (m_tab == 1))
            continue;
        switch (m_filter) {
        case Flagged: if (a.risk == 0) continue; break;
        case Sideloaded: if (!a.Has(AppFlag_Sideloaded)) continue; break;
        case NoIcon: if (!a.Has(AppFlag_NoIcon)) continue; break;
        case Uninstall: if (!session.IsFlaggedForUninstall(a.package)) continue; break;
        default: break;
        }
        if (!search.empty() && !IContains(a.package, search) && !IContains(a.label, search))
            continue;
        m_view.push_back(i);
    }
    std::ranges::sort(m_view, [&](int x, int y) {
        const auto& a = apps[x];
        const auto& b = apps[y];
        switch (m_sort) {
        case BySize: if (a.TotalBytes() != b.TotalBytes()) return a.TotalBytes() > b.TotalBytes(); break;
        case ByName: return a.DisplayName() < b.DisplayName();
        case ByInstalled: if (a.firstInstall != b.firstInstall) return a.firstInstall > b.firstInstall; break;
        default:
            if (a.risk != b.risk)
                return a.risk > b.risk;
            if (a.TotalBytes() != b.TotalBytes())
                return a.TotalBytes() > b.TotalBytes();
            break;
        }
        return a.package < b.package;
    });
    m_viewVersion = session.Version();
    m_dirty = false;
}

void AppsPage::Draw(AppContext& ctx)
{
    AppSession& session = ctx.apps;
    if (s_wantFlagged) {
        s_wantFlagged = false;
        m_tab = 0;
        m_filter = Flagged;
        m_dirty = true;
    }
    if (m_dirty || session.Version() != m_viewVersion)
        RebuildView(session);
    // Dev preset: --files=drawer opens the riskiest app's drawer once apps are in.
    if (ctx.filesPreset == "uninstall" && !m_view.empty() && !ctx.files.Files().empty()) { // dev: show the dialog only
        ctx.filesPreset.clear();
        for (int idx : m_view)
            if (session.Apps()[idx].package == "com.whatsapp" || idx == m_view.back()) {
                OpenUninstall(ctx, session.Apps()[idx], UninstallMode::Full);
                break;
            }
    }
    if (ctx.filesPreset == "drawer" && !m_view.empty()) {
        ctx.filesPreset.clear();
        m_drawer = session.Apps()[m_view.front()].package;
        session.RequestLabel(session.Apps()[m_view.front()]);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
        if (!m_drawer.empty())
            m_drawer.clear();
        else
            m_selected.clear();
    }

    const float h = ImGui::GetContentRegionAvail().y;
    const float drawerW = m_drawer.empty() ? 0.0f : S(380);
    const float mainW = ImGui::GetContentRegionAvail().x - (drawerW > 0 ? drawerW + S(12) : 0.0f);

    ImGui::BeginChild("##appsmain", ImVec2(mainW, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    DrawToolbar(ctx);
    ImGui::Dummy(ImVec2(0, S(4)));

    const bool hasSel = m_tab == 0 && !m_selected.empty();
    const float listH = ImGui::GetContentRegionAvail().y - (hasSel ? S(54) : 0.0f);
    const auto state = session.GetState();

    if (session.Apps().empty()) {
        ImGui::BeginChild("##appsstate", ImVec2(0, listH));
        if (state == AppSession::State::Running) {
            ui::SkeletonRows(std::max(3, static_cast<int>(listH / S(52))), S(52));
        } else if (state == AppSession::State::Failed) {
            ImGui::Dummy(ImVec2(0, S(24)));
            const auto o = session.Outcome();
            if (ui::ErrorCard("appserr", o.error, o.errorDetail, ICON_ROTATE "  Retry"))
                ctx.requestAppScan = true;
        } else {
            ui::EmptyState(ICON_CUBES, "No app scan yet", "Scan the phone to list its apps.");
            ImGui::Dummy(ImVec2(0, S(10)));
            const char* label = ICON_ROTATE "  Scan apps";
            const float bw = ui::TextWidth(label) + S(32);
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f);
            if (ui::PrimaryButton(label, ImVec2(bw, 0)))
                ctx.requestAppScan = true;
        }
        ImGui::EndChild();
    } else if (m_view.empty()) {
        ImGui::BeginChild("##appsempty", ImVec2(0, listH));
        if (m_filter == Flagged)
            ui::EmptyState(ICON_SHIELD, "No flagged apps \xF0\x9F\x8E\x89", "Nothing here raises a flag.");
        else
            ui::EmptyState(ICON_SEARCH, "No apps match", "Try another filter or search.");
        ImGui::EndChild();
    } else {
        DrawTable(ctx, listH);
    }
    if (hasSel)
        DrawSelectionBar(ctx);
    ImGui::EndChild();

    if (drawerW > 0) {
        ImGui::SameLine(0, S(12));
        DrawDrawer(ctx, h);
    }
    DrawUninstall(ctx);
}

void AppsPage::DrawToolbar(AppContext& ctx)
{
    AppSession& session = ctx.apps;
    // Tabs: User apps · System apps (view only).
    const size_t users = session.UserCount();
    const size_t systems = session.Apps().size() - users;
    auto tab = [&](int idx, const std::string& label) {
        const bool active = m_tab == idx;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? PanelRaised : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PanelHover);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Text : TextDim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(7)));
        if (ImGui::Button(label.c_str()) && !active) {
            m_tab = idx;
            m_selected.clear();
            m_dirty = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (active) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + S(8), b.y - S(2)), ImVec2(b.x - S(8), b.y), U32(Accent), S(1));
        }
    };
    tab(0, std::format("User apps  {}", users));
    ImGui::SameLine(0, S(4));
    tab(1, std::format("System apps  {}", systems));
    ImGui::SameLine(0, S(8));
    ImGui::AlignTextToFramePadding();
    if (m_tab == 1) {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText(ICON_LOCK "  view only: use UAD-ng for system apps");
    }

    // Right side of the tab row: sort + labels button.
    static constexpr const char* kSorts[] = { "Sort: Risk", "Sort: Size", "Sort: Name", "Sort: Newest" };
    const float sortW = S(130);
    std::string labelsText;
    float labelsW = 0;
    const size_t pending = session.LabelsPending();
    if (session.LabelsSupported()) {
        labelsText = pending ? std::format(ICON_ROTATE "  Reading names\xE2\x80\xA6 {}", pending) : std::string(ICON_FILE_LINES "  Read app names");
        labelsW = ui::TextWidth(labelsText) + S(28);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - sortW - (labelsW > 0 ? labelsW + S(8) : 0.0f)));
    ImGui::SetNextItemWidth(sortW);
    if (ImGui::BeginCombo("##sort", kSorts[m_sort])) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(kSorts[i], m_sort == i)) {
                m_sort = static_cast<SortBy>(i);
                m_dirty = true;
            }
        ImGui::EndCombo();
    }
    if (labelsW > 0) {
        ImGui::SameLine(0, S(8));
        if (ui::SecondaryButton(labelsText.c_str(), ImVec2(labelsW, 0), pending == 0 && !session.Apps().empty()))
            m_confirmLabels = true;
        ImGui::SetItemTooltip("Pulls each app's base.apk and reads its name with aapt2.\nNames are cached, so this is a one-time cost.");
    }

    // Search + filter chips; the search box gives up width first.
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(6)));
    struct Chip {
        Filter f;
        const char* label;
    };
    static constexpr Chip kChips[] = { { All, "All" }, { Flagged, ICON_WARNING " Flagged" }, { Sideloaded, "Sideloaded" },
        { NoIcon, "No icon" }, { Uninstall, ICON_TRASH " To uninstall" } };
    std::vector<std::string> chipText;
    float chipsW = 0;
    for (const auto& c : kChips) {
        if (m_tab == 1 && c.f == Uninstall)
            continue;
        std::string text = c.label;
        if (c.f == Uninstall && session.UninstallCount())
            text += std::format(" ({})", session.UninstallCount());
        chipsW += ui::TextWidth(text) + S(20) + S(4);
        chipText.push_back(std::move(text));
    }
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - chipsW - S(8), S(120), S(260)));
    if (ImGui::InputTextWithHint("##appsearch", ICON_SEARCH "  Search name or package", m_search, sizeof(m_search)))
        m_dirty = true;
    size_t ci = 0;
    for (const auto& c : kChips) {
        if (m_tab == 1 && c.f == Uninstall)
            continue;
        ImGui::SameLine(0, S(4));
        const bool active = m_filter == c.f;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? Hex(0x3DD6C6, 0.18f) : PanelRaised);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Accent : TextDim);
        if (ImGui::Button(chipText[ci++].c_str())) {
            m_filter = c.f;
            m_dirty = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();

    // Bulk label confirmation (it can pull a few GB).
    if (m_confirmLabels) {
        ImGui::OpenPopup("Read app names?");
        m_confirmLabels = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(460), 0));
    if (ImGui::BeginPopupModal("Read app names?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        uint64_t bytes = 0;
        size_t n = 0;
        for (const auto& a : session.Apps())
            if (!a.system && a.label.empty()) {
                bytes += a.codeBytes.value_or(0);
                ++n;
            }
        ImGui::TextWrapped("PhoneSweep will copy each user app's APK (about %s for %zu apps) to a temp folder, read its name "
                           "with aapt2, then delete the copy. It runs in the background; names are cached for next time.",
            ui::Bytes(bytes).c_str(), n);
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ui::PrimaryButton("Read names")) {
            session.RequestAllLabels(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void AppsPage::DrawTable(AppContext& ctx, float height)
{
    AppSession& session = ctx.apps;
    const auto& apps = session.Apps();
    const bool selectable = m_tab == 0;
    const float rowH = S(54);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8), 0));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
        | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##apps", 5, flags, ImVec2(0, height))) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, selectable ? S(26) : S(4));
    ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, S(90));
    ImGui::TableSetupColumn("Installer", ImGuiTableColumnFlags_WidthFixed, S(130));
    ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 3.2f);
    ImGui::TableHeadersRow();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_view.size()), rowH);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const AppInfo& a = apps[m_view[row]];
            const bool selected = m_selected.contains(a.package);
            const bool uninstall = session.IsFlaggedForUninstall(a.package);
            ImGui::TableNextRow(0, rowH);
            ImGui::PushID(a.package.c_str());

            ImGui::TableSetColumnIndex(0);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", selected || m_drawer == a.package,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH))) {
                // Click opens the drawer; the checkbox column toggles selection.
                const float mx = ImGui::GetIO().MousePos.x;
                if (selectable && mx < rowMin.x + S(26)) {
                    if (selected)
                        m_selected.erase(a.package);
                    else
                        m_selected.insert(a.package);
                } else {
                    m_drawer = a.package;
                    session.RequestLabel(a);
                }
            }
            // Right-click: quick actions for this app.
            if (ImGui::BeginPopupContextItem("##appmenu")) {
                session.RequestLabel(a);
                DrawContextMenu(ctx, a);
                ImGui::EndPopup();
            }
            if (selectable) {
                const float box = S(15);
                const ImVec2 bp(rowMin.x, rowMin.y + (rowH - box) * 0.5f);
                if (selected)
                    dl->AddRectFilled(bp, ImVec2(bp.x + box, bp.y + box), U32(Accent), S(3));
                else
                    dl->AddRect(bp, ImVec2(bp.x + box, bp.y + box), U32(TextDim, 0.7f), S(3), 0, 1.2f);
                if (selected)
                    dl->AddText(ImVec2(bp.x + S(1), bp.y - S(1)), U32(Background), ICON_CHECK);
            }

            // App: avatar + name + package.
            ImGui::TableSetColumnIndex(1);
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float av = S(34);
                Avatar(dl, ImVec2(p.x, p.y + (rowH - av) * 0.5f), av, a);
                const float tx = p.x + av + S(12);
                const float lh = ImGui::GetTextLineHeight();
                const float y0 = p.y + (rowH - lh * 2 - S(2)) * 0.5f;
                {
                    Font f(GetFonts().uiStrong, FontBase);
                    const std::string name = a.label.empty() ? std::string(a.package.substr(a.package.rfind('.') + 1)) : a.label;
                    dl->AddText(ImVec2(tx, y0), U32(a.label.empty() ? TextDim : Text), name.c_str());
                    if (uninstall) {
                        const float nw = ImGui::CalcTextSize(name.c_str()).x;
                        Font sf(GetFonts().uiStrong, FontSmall - 2);
                        dl->AddText(ImVec2(tx + nw + S(8), y0 + S(2)), U32(Danger), ICON_TRASH " UNINSTALL");
                    }
                }
                {
                    Font f(GetFonts().mono, FontSmall);
                    dl->AddText(ImVec2(tx, y0 + lh + S(2)), U32(TextDim), a.package.c_str());
                }
            }

            auto cellText = [&](const std::string& s, const ImVec4& col) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
                ui::ColoredText(col, s);
            };
            ImGui::TableSetColumnIndex(2);
            cellText(Size(a), Text);
            ImGui::TableSetColumnIndex(3);
            {
                const std::string inst = InstallerName(a);
                cellText(inst, a.Has(AppFlag_Sideloaded) ? Warning : TextDim);
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight() - S(4)) * 0.5f);
            FlagPills(a, false, ImGui::GetContentRegionAvail().x);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void AppsPage::DrawSelectionBar(AppContext& ctx)
{
    AppSession& session = ctx.apps;
    const auto sel = SelectedApps(session);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), 0));
    ImGui::BeginChild("##appsel", ImVec2(0, S(50)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    const float midY = S(50) * 0.5f;
    ImGui::SetCursorPosY(midY - ImGui::GetFrameHeight() * 0.5f);
    ImGui::AlignTextToFramePadding();
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(std::format("{} selected", sel.size()).c_str());
    }
    const bool allFlagged = !sel.empty() && std::ranges::all_of(sel, [&](const AppInfo* a) { return session.IsFlaggedForUninstall(a->package); });

    const char* backup = ICON_DOWNLOAD "  Backup APK";
    const char* flag = allFlagged ? "Unflag" : ICON_TRASH "  Flag for uninstall";
    const char* copy = ICON_COPY "  Copy for AI";
    const float total = ui::TextWidth(backup) + ui::TextWidth(flag) + ui::TextWidth(copy) + S(28) * 3 + S(8) * 2 + S(4);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));
    ImGui::SetCursorPosY(midY - (ImGui::GetFrameHeight() + S(4)) * 0.5f);
    if (ui::SecondaryButton(backup, {}, ctx.ActiveConnected()))
        for (const AppInfo* a : sel)
            ctx.requestSaveApks.push_back(a->package);
    ImGui::SetItemTooltip("Copies base.apk and splits to apks/<package>/ in the backup folder, verified.");
    ImGui::SameLine(0, S(8));
    if (ui::SecondaryButton(flag)) {
        for (const AppInfo* a : sel)
            session.SetFlaggedForUninstall(a->package, !allFlagged);
        ui::Toast(allFlagged ? std::format("Unflagged {} apps", sel.size())
                             : std::format("{} apps added to the Nuke plan (nothing uninstalled yet)", sel.size()));
    }
    ImGui::SameLine(0, S(8));
    if (ui::PrimaryButton(copy)) {
        ctx.platform.SetClipboardText(FormatAppList(sel));
        ui::Toast(std::format("Copied {} apps for your AI assistant", sel.size()));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void AppsPage::DrawDrawer(AppContext& ctx, float height)
{
    AppSession& session = ctx.apps;
    const AppInfo* found = FindApp(session, m_drawer);
    if (!found) {
        m_drawer.clear();
        return;
    }
    const AppInfo& a = *found;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(16)));
    ImGui::BeginChild("##appdrawer", ImVec2(S(380), height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    // Header.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float av = S(52);
        Avatar(ImGui::GetWindowDrawList(), p, av, a);
        ImGui::Dummy(ImVec2(av, av));
        ImGui::SameLine(0, S(12));
        ImGui::BeginGroup();
        {
            Font f(GetFonts().uiStrong, FontH2 - 4);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(30));
            ImGui::TextUnformatted(a.label.empty() ? "Name unknown" : a.label.c_str());
            ImGui::PopTextWrapPos();
        }
        if (a.label.empty() && session.LabelPending(a.package)) {
            Font f(GetFonts().ui, FontSmall);
            ui::DimText("Reading name from APK\xE2\x80\xA6");
        } else if (a.label.empty() && !session.LabelsSupported()) {
            Font f(GetFonts().ui, FontSmall);
            ui::DimText("Install Android SDK build-tools (aapt2) for names.");
        }
        {
            Font f(GetFonts().mono, FontSmall);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
            ui::DimText(a.package);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - S(24), p.y - S(4)));
        if (ui::GhostButton(ICON_XMARK "##closeapp"))
            m_drawer.clear();
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::BeginChild("##appdrawerbody", ImVec2(0, -S(98)));

    if (session.IsFlaggedForUninstall(a.package))
        ui::Pill(ICON_TRASH " IN NUKE PLAN: WILL BE UNINSTALLED", Danger, true);

    // Why flagged.
    if (a.flags) {
        ImGui::Dummy(ImVec2(0, S(4)));
        for (const auto& fi : AllFlags()) {
            if (!(a.flags & fi.flag))
                continue;
            ui::Pill(fi.label, SeverityColor(fi.severity));
            ImGui::SameLine(0, S(8));
            Font f(GetFonts().ui, FontSmall);
            ImGui::PushTextWrapPos(0.0f);
            ui::DimText(fi.why);
            ImGui::PopTextWrapPos();
        }
    }

    // Details.
    ImGui::Dummy(ImVec2(0, S(8)));
    auto meta = [&](const char* label, const std::string& value, const ImVec4* col = nullptr) {
        ui::DimText(label);
        ImGui::SameLine(S(120));
        ImGui::PushTextWrapPos(0.0f);
        if (col)
            ui::ColoredText(*col, value);
        else
            ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    };
    meta("Version", a.versionName.empty() ? "\xE2\x80\x94" : std::format("{}  ({})", a.versionName, a.versionCode));
    meta("Installed", a.firstInstall.empty() ? "\xE2\x80\x94" : a.firstInstall);
    meta("Updated", a.lastUpdate.empty() ? "\xE2\x80\x94" : a.lastUpdate);
    {
        const std::string inst = InstallerName(a);
        meta("Installer", inst == a.installer ? inst : (a.installer.empty() ? inst : std::format("{}  ({})", inst, a.installer)),
            a.Has(AppFlag_Sideloaded) ? &Warning : nullptr);
    }
    if (a.HasSize())
        meta("Size", std::format("{} app \xC2\xB7 {} data \xC2\xB7 {} cache", ui::Bytes(a.codeBytes.value_or(0)), ui::Bytes(a.dataBytes.value_or(0)),
                         ui::Bytes(a.cacheBytes.value_or(0))));
    meta("Launcher icon", a.launcher ? "yes" : "no");
    meta("Starts at boot", a.Has(AppFlag_Boot) ? "yes" : "no");
    if (a.targetSdk)
        meta("Target SDK", std::to_string(a.targetSdk));
    meta("UID", std::to_string(a.uid));
    if (const auto u = ctx.usage.Find(a.package)) {
        if (u->cpu)
            meta("CPU / RAM", std::format("{:.1f}% \xC2\xB7 {} (live)", *u->cpu, ui::Bytes(u->ramBytes.value_or(0))));
        if (u->batteryMah)
            meta("Battery", std::format("{:.1f} mAh since full charge{} (estimate)", *u->batteryMah,
                                u->batteryShare ? std::format(" \xC2\xB7 {:.1f}%", *u->batteryShare * 100.0) : std::string()));
    }

    // Permissions.
    ImGui::Dummy(ImVec2(0, S(8)));
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(std::format("Granted permissions ({})", a.grantedPermissions.size()).c_str());
    }
    {
        Font f(GetFonts().mono, FontSmall);
        // Sensitive first, highlighted.
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& p : a.grantedPermissions) {
                const bool sensitive = IsSensitivePermission(p);
                if ((pass == 0) != sensitive)
                    continue;
                std::string_view shown = p;
                if (shown.starts_with("android.permission."))
                    shown.remove_prefix(19);
                ui::ColoredText(sensitive ? Warning : TextDim, shown);
            }
        if (a.grantedPermissions.empty())
            ui::DimText("none");
    }

    // APK paths.
    ImGui::Dummy(ImVec2(0, S(8)));
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted("APK files");
    }
    {
        Font f(GetFonts().mono, FontSmall);
        ImGui::PushTextWrapPos(0.0f);
        for (const auto& p : a.apkPaths)
            ui::DimText(p);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    // Actions.
    ImGui::Dummy(ImVec2(0, S(4)));
    const float bw = (ImGui::GetContentRegionAvail().x - S(8)) * 0.5f;
    const bool userApp = !a.system;
    if (ui::SecondaryButton(ICON_POWER "  Force stop", ImVec2(bw, 0), ctx.ActiveConnected()))
        ctx.requestForceStop = a.package;
    ImGui::SameLine(0, S(8));
    if (ui::PrimaryButton(ICON_COPY "  Ask AI", ImVec2(bw, 0)))
        AskAi(ctx, a);
    ImGui::SetItemTooltip("Copies a question + everything known about this app (permissions, usage, battery) for any AI assistant.");
    if (ui::SecondaryButton(ICON_DOWNLOAD "  Backup APK", ImVec2(bw, 0), ctx.ActiveConnected() && !a.apkPaths.empty())) {
        ctx.requestSaveApks.push_back(a.package);
    }
    ImGui::SetItemTooltip("Copies base.apk and splits to apks/<package>/ in the backup folder, verified.");
    ImGui::SameLine(0, S(8));
    if (userApp) {
        const bool flagged = session.IsFlaggedForUninstall(a.package);
        if (flagged ? ui::SecondaryButton("Unflag uninstall", ImVec2(bw, 0)) : ui::DangerButton(ICON_TRASH "  Flag for uninstall", ImVec2(bw, 0))) {
            session.SetFlaggedForUninstall(a.package, !flagged);
            if (!flagged)
                ui::Toast("Added to the Nuke plan (nothing uninstalled yet)", ui::ToastKind::Info);
        }
        ImGui::SetItemTooltip("Adds this app to the Nuke plan. Uninstalling only happens from Nuke, after you confirm.");
    } else {
        ui::SecondaryButton(ICON_LOCK "  System app", ImVec2(bw, 0), false);
        ImGui::SetItemTooltip("System apps are view-only. UAD-ng handles debloating them.");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}


// ---- Right-click menu, Ask AI, uninstall ---------------------------------------------

void AppsPage::AskAi(AppContext& ctx, const AppInfo& a)
{
    const DeviceInfo& d = ctx.activeInfo;
    const std::string phone = d.displayName.empty() ? std::string() : std::format("{}, Android {}", d.displayName, d.android);
    ctx.platform.SetClipboardText(FormatAskAi(a, ctx.usage.Find(a.package), phone));
    ui::Toast(std::format("Copied {}: paste it into your AI assistant", a.DisplayName()));
}

void AppsPage::DrawContextMenu(AppContext& ctx, const AppInfo& a)
{
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(std::string(a.DisplayName()).c_str());
    }
    {
        Font f(GetFonts().mono, FontSmall);
        ui::DimText(a.package);
    }
    ImGui::Separator();
    const std::string play = PlayStoreUrl(a.package);
    if (ImGui::MenuItem(ICON_SEARCH "  Search on Google Play", nullptr, false, !play.empty()))
        ctx.platform.OpenUrl(play);
    if (ImGui::MenuItem(ICON_COPY "  Ask AI about this app"))
        AskAi(ctx, a);
    if (ImGui::MenuItem(ICON_COPY "  Copy package name"))
        ctx.platform.SetClipboardText(a.package);
    if (ImGui::MenuItem(ICON_FILES "  Show details"))
        m_drawer = a.package;
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_POWER "  Force stop", nullptr, false, ctx.ActiveConnected()))
        ctx.requestForceStop = a.package;
    if (ImGui::MenuItem(ICON_DOWNLOAD "  Back up APK", nullptr, false, ctx.ActiveConnected() && !a.apkPaths.empty()))
        ctx.requestSaveApks.push_back(a.package);
    ImGui::Separator();
    const bool can = !a.system && ctx.ActiveConnected() && !(m_uninstallJob && !m_uninstallJob->Finished());
    ImGui::PushStyleColor(ImGuiCol_Text, can ? Danger : TextDim);
    if (ImGui::MenuItem(ICON_TRASH "  Quick uninstall", nullptr, false, can))
        OpenUninstall(ctx, a, UninstallMode::Quick);
    if (ImGui::MenuItem(ICON_FIRE "  Full uninstall (data, cache, files)", nullptr, false, can))
        OpenUninstall(ctx, a, UninstallMode::Full);
    ImGui::PopStyleColor();
    if (a.system) {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText("System apps can't be uninstalled here.");
    }
}

void AppsPage::OpenUninstall(AppContext& ctx, const AppInfo& a, UninstallMode mode)
{
    m_uninstallPkg = a.package;
    m_uninstallMode = mode;
    m_backupApk = ctx.settings.backupApksBeforeUninstall;
    m_appFolders = AppStorageFolders(a.package, ctx.files, ctx.verified);
    m_uninstall.reset();
    m_uninstallOpen = true;
}

void AppsPage::DrawUninstall(AppContext& ctx)
{
    if (m_uninstallOpen) {
        ImGui::OpenPopup("Uninstall app");
        m_uninstallOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(560), 0));
    if (!ImGui::BeginPopupModal("Uninstall app", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const AppInfo* found = FindApp(ctx.apps, m_uninstallPkg);
    const bool full = m_uninstallMode == UninstallMode::Full;

    // Running / finished.
    if (m_uninstall) {
        std::string step, message;
        bool finished, ok;
        std::vector<std::string> removed;
        {
            auto keep = m_uninstall;
            std::lock_guard l(keep->mutex);
            step = keep->step;
            message = keep->message;
            finished = keep->finished;
            ok = keep->ok;
            removed = keep->removedFolders;
        }
        {
            Font f(GetFonts().uiStrong, FontBase + 1);
            ImGui::TextUnformatted(std::format("{} uninstall: {}", full ? "Full" : "Quick", m_uninstallPkg).c_str());
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        if (!finished) {
            ui::Spinner(S(7), S(2), Accent);
            ImGui::SameLine(0, S(8));
            ui::DimText(step.empty() ? std::string("Starting\xE2\x80\xA6") : step + "\xE2\x80\xA6");
        } else {
            ImGui::PushTextWrapPos(S(520));
            ui::ColoredText(ok ? Success : Warning, std::string(ok ? ICON_CIRCLE_CHECK "  " : ICON_WARNING "  ") + message);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, S(8)));
            if (ui::PrimaryButton("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (ok) {
                    // Drop the files that went with the app from the scan, then refresh the app list.
                    std::vector<std::string> gone;
                    for (const auto& e : ctx.files.Files())
                        for (const auto& folder : removed)
                            if (e.path.starts_with(folder)) {
                                gone.push_back(e.path);
                                break;
                            }
                    ctx.files.RemovePaths(gone);
                    ctx.apps.SetFlaggedForUninstall(m_uninstallPkg, false);
                    ctx.requestAppScan = true;
                    if (m_drawer == m_uninstallPkg)
                        m_drawer.clear();
                    m_selected.erase(m_uninstallPkg);
                }
                m_uninstall.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
        return;
    }
    if (!found) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const AppInfo& a = *found;

    // Confirmation.
    {
        Font f(GetFonts().uiStrong, FontH2 - 4);
        ImGui::TextUnformatted(std::format("{} {}?", full ? "Fully uninstall" : "Uninstall", a.DisplayName()).c_str());
    }
    {
        Font f(GetFonts().mono, FontSmall);
        ui::DimText(a.package);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushTextWrapPos(S(530));
    if (full) {
        ImGui::TextUnformatted("1. Back up the APK to the PC (if ticked below)\n2. Clear the app's data and cache\n3. Uninstall it\n"
                               "4. Delete its folders on the phone's storage:");
    } else {
        ImGui::TextUnformatted("Removes the app and its private data. Android also deletes the app's own folders (Android/data, Android/obb).");
    }
    ImGui::PopTextWrapPos();

    // The app's folders, with what's unsaved.
    auto shortPath = [&](const std::string& p) {
        for (const auto& v : ctx.files.Volumes())
            if (p.starts_with(v.root))
                return v.label + " \xC2\xB7 " + p.substr(v.root.size());
        return p;
    };
    if (!m_appFolders.empty()) {
        ImGui::Dummy(ImVec2(0, S(4)));
        Font f(GetFonts().ui, FontSmall);
        for (const auto& fo : m_appFolders) {
            ImGui::Bullet();
            ImGui::TextUnformatted(std::format("{}  \xC2\xB7  {} files, {}", shortPath(fo.path), ui::Count(fo.files), ui::Bytes(fo.bytes)).c_str());
            if (fo.unsaved && fo.kind != "obb") {
                ImGui::SameLine();
                ui::ColoredText(Warning, std::format("({} not saved)", ui::Count(fo.unsaved)));
            }
        }
    } else if (full) {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText("    (none found in the last scan)");
    }

    // Nuke Protector.
    const auto mode = static_cast<ProtectorMode>(std::clamp(ctx.settings.nukeMode, 0, 1));
    uint64_t unsavedBytes = 0;
    const size_t unsaved = UnsavedAppFiles(m_appFolders, &unsavedBytes);
    const bool blocked = mode == ProtectorMode::Prompt && unsaved > 0;
    ImGui::Dummy(ImVec2(0, S(6)));
    if (blocked) {
        ImGui::PushTextWrapPos(S(530));
        ui::ColoredText(Accent, std::format(ICON_SHIELD "  Nuke prompt: {} files ({}) in this app's folders aren't saved on the PC. "
                                            "Save them first, or switch to Nuke free in Settings.",
                                    ui::Count(unsaved), ui::Bytes(unsavedBytes)));
        ImGui::PopTextWrapPos();
        if (ui::SecondaryButton(std::format(ICON_DOWNLOAD "  Save {} files first", ui::Count(unsaved)).c_str(), {}, ctx.ActiveConnected())) {
            const auto& files = ctx.files.Files();
            for (uint32_t i = 0; i < files.size(); ++i)
                for (const auto& fo : m_appFolders)
                    if (fo.kind != "obb" && files[i].path.starts_with(fo.path) && !ctx.verified.IsVerified(files[i].path, files[i].size, files[i].mtime)) {
                        ctx.requestSaveFiles.push_back(i);
                        break;
                    }
            ui::Toast("Saving them to the PC; uninstall unlocks once they're verified", ui::ToastKind::Info);
            ImGui::CloseCurrentPopup();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::Checkbox("Back up the APK to the PC first", &m_backupApk);
    ImGui::SetItemTooltip("Copies base.apk + splits to apks/<package>/ in today's backup folder. If the copy fails, nothing is uninstalled.");
    ImGui::Dummy(ImVec2(0, S(8)));

    const std::string go = full ? ICON_FIRE "  Fully uninstall" : ICON_TRASH "  Uninstall";
    if (ui::DangerButton(go.c_str(), {}, !blocked && ctx.ActiveConnected() && ctx.adb)) {
        UninstallTask t;
        t.serial = ctx.activeSerial;
        t.app = a;
        t.mode = m_uninstallMode;
        t.backupApk = m_backupApk;
        t.apkDir = ctx.saveFolder / L"apks";
        for (const auto& fo : m_appFolders)
            t.folders.push_back(fo.path);
        for (const auto& e : ctx.files.Files())
            for (const auto& fo : m_appFolders)
                if (e.path.starts_with(fo.path)) {
                    t.knownFiles.push_back(e.path);
                    break;
                }
        m_uninstall = std::make_shared<UninstallProgress>();
        auto progress = m_uninstall;
        auto adb = ctx.adb;
        m_uninstallJob = ctx.jobs.Submit(full ? "Full uninstall" : "Uninstalling", JobPriority::High,
            [adb, t = std::move(t), progress](Job&, std::stop_token st) { RunUninstall(*adb, t, *progress, st); });
    }
    ImGui::SameLine();
    if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace ps
