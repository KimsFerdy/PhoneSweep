#include "screens/DupCompare.h"

#include "adb/AdbClient.h"
#include "app/AppContext.h"
#include "cleanup/Preview.h"
#include "platform/Platform.h"
#include "scan/FileStore.h"
#include "screens/ThumbArea.h"
#include "thumbs/ThumbCache.h"
#include "transfer/Manifest.h"
#include "transfer/PhoneIO.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Process.h"
#include "util/Utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

constexpr const char* kPopup = "##dupcompare";
constexpr const char* kConfirm = "Delete these copies?";

} // namespace

DupCompare::DupCompare() = default;
DupCompare::~DupCompare() = default;

void DupCompare::Open(int group)
{
    m_group = group;
    m_justOpened = true;
}

void DupCompare::Close()
{
    m_group = -1;
    m_confirm = false;
    m_kit.Release();
}


void DupCompare::Draw(AppContext& ctx, std::vector<DupGroup>& groups, std::vector<bool>& ticked, const Lookup& lookup)
{
    if (m_group < 0 || m_group >= static_cast<int>(groups.size())) {
        // Closed from outside, or the group vanished (rescan): close the popup properly so the modal stops blocking.
        if (ImGui::BeginPopupModal(kPopup)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (m_group >= 0)
            Close();
        return;
    }
    if (m_justOpened) {
        ImGui::OpenPopup(kPopup);
        m_justOpened = false;
    }
    m_kit.Pump(ctx);

    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::max(S(640), work.x - S(80)), std::max(S(480), work.y - S(80))), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    const bool visible = ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!visible) {
        Close();
        return;
    }

    DupGroup& grp = groups[static_cast<size_t>(m_group)];
    bool tick = ticked[static_cast<size_t>(m_group)];
    const int count = static_cast<int>(groups.size());
    int go = m_group;

    // Header: title, group navigation, close.
    {
        Font f(GetFonts().uiStrong, FontH2 - 2);
        ImGui::TextUnformatted(ICON_COLUMNS "  Compare copies");
    }
    ImGui::SameLine(0, S(14));
    ImGui::AlignTextToFramePadding();
    ui::DimText(std::format("Group {} of {}  \xC2\xB7  {} identical copies  \xC2\xB7  {} each", m_group + 1, count, grp.files.size(), ui::Bytes(grp.size)));
    {
        const float bw = S(34);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 3 - S(16));
        if (ui::SecondaryButton(ICON_CHEVRON_LEFT "##prev", ImVec2(bw, 0), m_group > 0))
            go = m_group - 1;
        ImGui::SetItemTooltip("Previous group (\xE2\x86\x90)");
        ImGui::SameLine(0, S(4));
        if (ui::SecondaryButton(ICON_CHEVRON_RIGHT "##next", ImVec2(bw, 0), m_group + 1 < count))
            go = m_group + 1;
        ImGui::SetItemTooltip("Next group (\xE2\x86\x92)");
        ImGui::SameLine(0, S(12));
        if (ui::GhostButton(ICON_XMARK "##closecmp", ImVec2(bw, 0)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_confirm && !ImGui::IsPopupOpen(kConfirm))) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            Close();
            return;
        }
    }
    {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText("These files are byte-for-byte identical (same MD5). Mark the copies you don't need with Delete, "
                    "then press \"Delete now\" (or use Delete in the list for all groups at once).");
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    // Columns, side by side (scrolls sideways when there are many copies).
    const float footerH = S(52);
    const float colsH = ImGui::GetContentRegionAvail().y - footerH;
    const size_t n = grp.files.size();
    const float gap = S(12);
    const float availW = ImGui::GetContentRegionAvail().x;
    const float colW = std::max(S(250), (availW - gap * static_cast<float>(n - 1)) / static_cast<float>(n));
    ImGui::BeginChild("##cols", ImVec2(0, colsH), 0, ImGuiWindowFlags_HorizontalScrollbar);
    const float colH = ImGui::GetContentRegionAvail().y - (colW * static_cast<float>(n) > availW ? ImGui::GetStyle().ScrollbarSize : 0.0f);
    for (size_t i = 0; i < n; ++i) {
        if (i)
            ImGui::SameLine(0, gap);
        ImGui::PushID(static_cast<int>(i));
        DrawColumn(ctx, grp, tick, i, lookup(grp.files[i].path), colW, colH);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Footer: what this group will do + quick choices.
    ImGui::Dummy(ImVec2(0, S(6)));
    {
        const size_t kept = tick ? grp.KeptCount() : n;
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::AlignTextToFramePadding();
        if (!m_notice.empty() && ImGui::GetTime() < m_noticeUntil) {
            ui::ColoredText(m_notice.starts_with(ICON_WARNING) ? Warning : Success, m_notice);
            ImGui::SameLine(0, S(8));
        }
        if (tick && kept < n)
            ui::ColoredText(Danger, std::format("Keeps {}  \xC2\xB7  deletes {}  \xC2\xB7  frees {}", kept, n - kept, ui::Bytes(grp.Reclaimable())));
        else
            ui::ColoredText(Accent, "Keeps every copy (this group is skipped)");
    }
    ImGui::SameLine();
    {
        // Deletes sent from here: wait for the phone, then they drop out of the group.
        const bool deleting = !m_deleting.empty() && (ctx.deleteRunning || ctx.requestDelete || ImGui::GetFrameCount() - m_sentFrame < 3);
        if (!m_deleting.empty() && !deleting) {
            // Count what really left the phone (the file list drops deleted files).
            size_t still = 0;
            for (const auto& fe : ctx.files.Files())
                still += std::ranges::find(m_deleting, fe.path) != m_deleting.end();
            const size_t gone = m_deleting.size() - std::min(still, m_deleting.size());
            m_notice = gone == 0 ? std::string(ICON_WARNING "  Nothing was deleted (see the message)")
                                 : std::format(ICON_CIRCLE_CHECK "  Deleted {} {} · freed {}", gone, gone == 1 ? "copy" : "copies",
                                       ui::Bytes(m_deletingBytes / m_deleting.size() * gone));
            m_noticeUntil = ImGui::GetTime() + 5.0;
            m_deleting.clear();
        }
        const size_t toDelete = tick ? n - std::min(n, grp.KeptCount()) : 0;
        const std::string delLabel = deleting ? std::string("Deleting\xE2\x80\xA6")
                                              : std::format(ICON_TRASH "  Delete now ({})", ui::Bytes(grp.size * toDelete));
        const char* keepAll = ICON_CHECK "  Keep all copies";
        const char* done = "Close";
        const float w = ui::TextWidth(keepAll) + ui::TextWidth(delLabel) + ui::TextWidth(done) + S(110);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
        if (ui::SecondaryButton(keepAll, {}, tick && !deleting))
            tick = false;
        ImGui::SetItemTooltip("Skip this group: nothing in it is deleted.");
        ImGui::SameLine();
        if (ui::DangerButton(delLabel.c_str(), {}, toDelete > 0 && !deleting && !ctx.deleteRunning && ctx.ActiveConnected()))
            m_confirm = true;
        if (toDelete == 0)
            ImGui::SetItemTooltip("Mark at least one copy with Delete first.");
        else if (!ctx.ActiveConnected())
            ImGui::SetItemTooltip("The phone isn't connected.");
        ImGui::SameLine();
        if (ui::PrimaryButton(done)) {
            ImGui::CloseCurrentPopup();
            ticked[static_cast<size_t>(m_group)] = tick;
            ImGui::EndPopup();
            Close();
            return;
        }
    }

    // Arrow keys switch groups.
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && m_group > 0)
            go = m_group - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && m_group + 1 < count)
            go = m_group + 1;
    }
    ticked[static_cast<size_t>(m_group)] = tick;
    DrawConfirm(ctx, grp);
    if (go != m_group) {
        m_kit.StopAudio();
        m_group = go;
    }
    ImGui::EndPopup();
}

void DupCompare::SendDelete(AppContext& ctx, const DupGroup& grp)
{
    DeleteRequest req;
    req.paths = DuplicateDeleteList({ grp }, { true }, ctx.files);
    if (req.paths.empty())
        return;
    for (size_t k = 0; k < grp.files.size(); ++k)
        if (grp.Keeps(k))
            req.mustExist.push_back(grp.files[k].path);
    req.label = req.paths.size() == 1 ? "duplicate copy" : "duplicate copies";
    m_deleting = req.paths;
    m_deletingBytes = grp.size * req.paths.size();
    m_sentFrame = ImGui::GetFrameCount();
    ctx.requestDelete = std::move(req);
}

void DupCompare::DrawConfirm(AppContext& ctx, DupGroup& grp)
{
    if (m_confirm) {
        ImGui::OpenPopup(kConfirm);
        m_confirm = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(480), 0));
    if (!ImGui::BeginPopupModal(kConfirm, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;
    const auto paths = DuplicateDeleteList({ grp }, { true }, ctx.files);
    ImGui::TextWrapped("Delete %zu %s (%s) from the phone? This can't be undone.", paths.size(), paths.size() == 1 ? "copy" : "copies",
        ui::Bytes(grp.size * paths.size()).c_str());
    {
        Font f(GetFonts().ui, FontSmall);
        for (const auto& p : paths)
            ui::ColoredText(Danger, std::string(ICON_TRASH "  ") + FolderCaption(p) + "/" + p.substr(p.rfind('/') + 1));
        const size_t kept = grp.files.size() - paths.size();
        ui::DimText(std::format("{} {} stay{}; PhoneSweep checks {} still there first.", kept, kept == 1 ? "copy" : "copies", kept == 1 ? "s" : "",
            kept == 1 ? "it's" : "they're"));
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    if (ui::DangerButton(ICON_TRASH "  Delete", {}, !paths.empty())) {
        SendDelete(ctx, grp);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DupCompare::DrawColumn(AppContext& ctx, DupGroup& grp, bool& tick, size_t i, const FileEntry* e, float width, float height)
{
    const bool keepFlag = e && (e->flags & FileFlag_Keep);
    const bool kept = !tick || grp.Keeps(i);
    const ImVec4 edge = kept ? Accent : Danger;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelRaised);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(edge.x, edge.y, edge.z, kept ? 0.35f : 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(RoundPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(12)));
    ImGui::BeginChild("##col", ImVec2(width, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    const DupFile& df = grp.files[i];
    const float w = ImGui::GetContentRegionAvail().x;
    const float infoH = S(150);
    const float ph = std::max(S(120), ImGui::GetContentRegionAvail().y - infoH);

    // Preview.
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + w, a.y + ph);
    if (e)
        m_kit.Draw(ctx, *e, a, b);
    else
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, U32(Border, 0.4f), S(6));
    {
        // Banner across the top of the preview: what happens to this copy.
        const bool deleting = std::ranges::find(m_deleting, grp.files[i].path) != m_deleting.end();
        if (!kept || deleting) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            Font f(GetFonts().uiStrong, FontSmall);
            const std::string label = deleting ? std::string("Deleting from the phone\xE2\x80\xA6") : std::string(ICON_TRASH "  WILL BE DELETED");
            const float bh = ImGui::GetTextLineHeight() + S(10);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + bh), U32(Danger, deleting ? 0.95f : 0.85f), S(6), ImDrawFlags_RoundCornersTop);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + S(5)), U32(Background), label.c_str());
            if (deleting)
                dl->AddRectFilled(ImVec2(a.x, a.y + bh), b, U32(Background, 0.45f));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + S(8)));

    // Name, folder, details.
    const std::string name = df.path.substr(df.path.rfind('/') + 1);
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(a.x + w, ImGui::GetCursorScreenPos().y + S(40)), true);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopClipRect();
    }
    ImGui::SetItemTooltip("%s", df.path.c_str());
    {
        Font f(GetFonts().ui, FontSmall);
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(a.x + w, ImGui::GetCursorScreenPos().y + S(40)), true);
        ui::DimText(FolderCaption(df.path));
        ImGui::PopClipRect();
        std::string details = ui::Date(df.mtime);
        if (e && e->source != Source::Other)
            details += std::format("  \xC2\xB7  {}", SourceName(e->source));
        ui::DimText(details);
    }
    // Chips.
    bool chip = false;
    if (e && ctx.verified.IsVerified(e->path, e->size, e->mtime)) {
        ui::Pill(ICON_CHECK " SAVED ON PC", Success);
        chip = true;
    }
    if (keepFlag) {
        if (chip)
            ImGui::SameLine(0, S(6));
        ui::Pill("KEEP", Accent);
        chip = true;
    }
    if (!chip)
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() + S(4)));

    // Keep / Delete.
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - S(12)));
    const float bw = (w - S(6)) * 0.5f;
    if (kept ? ui::PrimaryButton(ICON_CHECK "  Keep", ImVec2(bw, 0)) : ui::SecondaryButton("Keep", ImVec2(bw, 0))) {
        if (!kept) {
            grp.SetKept(i, true);
            if (grp.KeptCount() >= grp.files.size())
                tick = false; // nothing left to delete in this group
        }
    }
    ImGui::SameLine(0, S(6));
    if (!kept ? ui::DangerButton(ICON_TRASH "  Delete", ImVec2(bw, 0)) : ui::SecondaryButton("Delete", ImVec2(bw, 0), !keepFlag)) {
        if (kept) {
            if (!tick) {
                // Turning the group on from "keep everything": the others stay.
                for (size_t k = 0; k < grp.files.size(); ++k)
                    grp.SetKept(k, true);
                tick = true;
            }
            if (!grp.SetKept(i, false))
                ui::Toast("One copy always stays", ui::ToastKind::Info);
        }
    }
    if (keepFlag)
        ImGui::SetItemTooltip("Marked KEEP: never deleted.");
    ImGui::EndChild();
}

} // namespace ps
