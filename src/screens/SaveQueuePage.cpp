#include "screens/SaveQueuePage.h"

#include "app/AppContext.h"
#include "platform/Platform.h"
#include "transfer/Puller.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Utf8.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

std::string Eta(double seconds)
{
    if (seconds < 0)
        return "\xE2\x80\x94";
    const int s = static_cast<int>(seconds + 0.5);
    if (s < 60)
        return std::format("{}s", s);
    if (s < 3600)
        return std::format("{}m {:02}s", s / 60, s % 60);
    return std::format("{}h {:02}m", s / 3600, (s % 3600) / 60);
}

struct StateLook {
    const char* icon;
    ImVec4 color;
};

StateLook Look(SaveState s)
{
    switch (s) {
    case SaveState::Queued: return { ICON_CHEVRON_RIGHT, TextDim };
    case SaveState::Pulling: return { ICON_DOWNLOAD, Accent };
    case SaveState::Verifying: return { ICON_SHIELD, Info };
    case SaveState::Done: return { ICON_CIRCLE_CHECK, Success };
    case SaveState::Skipped: return { ICON_CIRCLE_CHECK, Success };
    case SaveState::Failed: return { ICON_WARNING, Danger };
    }
    return { ICON_FILE, TextDim };
}

} // namespace

void SaveQueuePage::Draw(AppContext& ctx)
{
    SaveQueue& q = ctx.saveQueue;
    const auto st = q.GetStats();
    const auto folder = q.Folder();

    if (st.total == 0) {
        ui::EmptyState(ICON_DOWNLOAD, "Nothing queued yet",
            "Select files in Files and click Save selected, or mark files KEEP and use \"Save everything important\" on Overview.");
        if (!folder.empty()) {
            ImGui::Dummy(ImVec2(0, S(8)));
            Font f(GetFonts().ui, FontSmall);
            ui::TextCentered(std::format("Backups go to {}", Narrow(folder.wstring())), &TextDim);
        }
        return;
    }

    // ---- Header: progress + controls ----
    ui::BeginPanel("##qhead", ICON_DOWNLOAD "  Save to PC", ImVec2(0, S(150)));
    {
        const double frac = st.bytesTotal ? static_cast<double>(st.bytesDone) / static_cast<double>(st.bytesTotal) : 0.0;
        const size_t finished = st.done + st.skipped;
        {
            Font f(GetFonts().uiStrong, FontH2 - 2);
            ImGui::TextUnformatted(std::format("{} / {} files", finished, st.total).c_str());
        }
        ImGui::SameLine(0, S(14));
        ImGui::AlignTextToFramePadding();
        ui::DimText(std::format("{} of {}", ui::Bytes(st.bytesDone), ui::Bytes(st.bytesTotal)));
        if (st.status == SaveQueue::Status::Running) {
            ImGui::SameLine(0, S(14));
            ui::ColoredText(Accent, std::format("{}/s", ui::Bytes(static_cast<uint64_t>(st.bytesPerSec))));
            ImGui::SameLine(0, S(14));
            ui::DimText("ETA " + Eta(st.etaSeconds));
        }
        if (st.failed) {
            ImGui::SameLine(0, S(14));
            ui::ColoredText(Danger, std::format(ICON_WARNING " {} failed", st.failed));
        }

        ui::ProgressBarThin(static_cast<float>(frac), ImGui::GetContentRegionAvail().x, S(8),
            st.failed && st.status != SaveQueue::Status::Running ? Warning : Accent);
        ImGui::Dummy(ImVec2(0, S(4)));

        // Controls.
        const bool running = st.status == SaveQueue::Status::Running;
        const bool paused = st.status == SaveQueue::Status::Paused || st.status == SaveQueue::Status::NoSpace;
        if (running) {
            if (ui::SecondaryButton(ICON_POWER "  Pause"))
                q.Pause();
        } else if (paused) {
            if (ui::PrimaryButton(ICON_ROTATE "  Resume"))
                q.Resume();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton(ICON_XMARK "  Cancel all", {}, st.queued + st.active > 0))
            q.CancelAll();
        ImGui::SameLine();
        if (ui::SecondaryButton(ICON_ROTATE "  Retry failed", {}, st.failed > 0))
            q.RetryFailed();
        ImGui::SameLine();
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Open folder", {}, !folder.empty())) {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            ctx.platform.OpenInExplorer(folder);
        }
        ImGui::SameLine();
        if (ui::GhostButton("Clear finished") && st.queued + st.active == 0)
            q.ClearFinished();
        ImGui::SetItemTooltip("Removes saved files from this list (they stay saved and verified).");
    }
    ui::EndPanel();
    ImGui::Dummy(ImVec2(0, S(8)));

    // ---- Status banners ----
    auto banner = [&](const ImVec4& col, const std::string& text) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetTextLineHeight() + S(16);
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(col, 0.12f), S(RoundButton));
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + S(12), p.y + S(8)), U32(col), text.c_str());
        ImGui::Dummy(ImVec2(w, h + S(6)));
    };
    switch (st.status) {
    case SaveQueue::Status::WaitingForDevice:
        banner(Warning, ICON_LINK_SLASH "  Phone disconnected: transfers paused. Files in progress go back in the queue; reconnect to continue.");
        break;
    case SaveQueue::Status::NoSpace:
        banner(Danger, ICON_WARNING "  " + st.lastError + ". Free some space, then Resume.");
        break;
    case SaveQueue::Status::Paused:
        banner(Info, ICON_INFO "  Paused. Nothing in progress was lost; Resume to continue.");
        break;
    case SaveQueue::Status::Done:
        if (st.failed == 0)
            banner(Success, std::format(ICON_CIRCLE_CHECK "  Saved {} files \xC2\xB7 {} \xC2\xB7 all verified{}", st.done + st.skipped,
                                ui::Bytes(st.bytesDone), q.Options().verifyMd5 ? " (size + MD5)" : " (size)"));
        else
            banner(Warning, std::format(ICON_WARNING "  Saved {} files; {} failed. Retry failed, or check the log for details.", st.done + st.skipped, st.failed));
        break;
    default: break;
    }

    // ---- File list ----
    const float rowH = S(40);
    const size_t count = q.ItemCount();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8), 0));
    if (ImGui::BeginTable("##queue", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##st", ImGuiTableColumnFlags_WidthFixed, S(26));
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, S(90));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count), rowH);
        while (clipper.Step()) {
            q.Visit(static_cast<size_t>(clipper.DisplayStart), static_cast<size_t>(clipper.DisplayEnd), [&](size_t, const SaveItem& it) {
                ImGui::TableNextRow(0, rowH);
                const StateLook look = Look(it.state);
                const float ty = (rowH - ImGui::GetTextLineHeight()) * 0.5f;

                ImGui::TableSetColumnIndex(0);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                ui::ColoredText(look.color, look.icon);

                ImGui::TableSetColumnIndex(1);
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const auto& dp = it.req.devicePath;
                    const std::string name = dp.substr(dp.rfind('/') + 1);
                    const std::string rel = Narrow(it.req.localRel.parent_path().generic_wstring());
                    const float lh = ImGui::GetTextLineHeight();
                    const float y0 = p.y + (rowH - lh * 2) * 0.5f;
                    dl->AddText(ImVec2(p.x, y0), U32(Text), name.c_str());
                    Font f(GetFonts().ui, FontSmall);
                    dl->AddText(ImVec2(p.x, y0 + lh), U32(TextDim), (it.req.kind == "apk" ? std::string("APK backup \xC2\xB7 ") + rel : rel).c_str());
                    ImGui::Dummy(ImVec2(0, rowH));
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                ui::DimText(it.req.size ? ui::Bytes(it.req.size) : std::string("\xE2\x80\x94"));

                ImGui::TableSetColumnIndex(3);
                if (it.state == SaveState::Pulling && it.req.size > 0) {
                    const float w = ImGui::GetContentRegionAvail().x - S(8);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - S(6)) * 0.5f - S(6));
                    {
                        Font f(GetFonts().ui, FontSmall);
                        ui::ColoredText(Accent, std::format("pulling {:.0f}%", 100.0 * static_cast<double>(it.bytesDone) / static_cast<double>(it.req.size)));
                    }
                    ui::ProgressBarThin(static_cast<float>(static_cast<double>(it.bytesDone) / static_cast<double>(it.req.size)), w, S(4), Accent);
                } else {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                    std::string text = ToString(it.state);
                    if (it.state == SaveState::Failed && !it.error.empty())
                        text += ": " + it.error;
                    else if (it.state == SaveState::Queued && it.attempts > 0)
                        text += std::format(" (retry {}: {})", it.attempts, it.error);
                    else if (it.state == SaveState::Done && q.Options().verifyMd5)
                        text += " \xC2\xB7 MD5 verified";
                    ui::ColoredText(look.color, text);
                    if (ImGui::IsItemHovered() && !it.finalPath.empty())
                        ImGui::SetTooltip("%s", Narrow(it.finalPath.wstring()).c_str());
                }
            });
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

} // namespace ps
