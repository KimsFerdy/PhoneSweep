#include "screens/ReportsPage.h"

#include "app/AppContext.h"
#include "platform/Platform.h"
#include "scan/AppSession.h"
#include "scan/FileStore.h"
#include "scan/ScanSession.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <imgui.h>

#include <format>

namespace ps {

using namespace ps::theme;

void ReportsPage::Draw(AppContext& ctx)
{
    const auto root = ctx.settings.ReportsRoot();
    // Refresh the list every couple of seconds, or right after an export.
    if (root != m_listedRoot || ImGui::GetTime() - m_lastList > 2.0) {
        m_exports = ListExports(root);
        m_listedRoot = root;
        m_lastList = ImGui::GetTime();
    }

    const bool haveData = !ctx.files.Files().empty() || !ctx.apps.Apps().empty();
    // Dev preset (--files=export): export once both scans are complete.
    if (ctx.filesPreset == "export" && !ctx.files.Files().empty() && !ctx.apps.Apps().empty() && !ctx.scan.Running() && !ctx.apps.Running()) {
        ctx.filesPreset.clear();
        ctx.requestExport = Export_All;
    }
    ui::BeginPanel("##exporthead", ICON_FILE_EXPORT "  Reports for AI", ImVec2(0, 0));
    ImGui::PushTextWrapPos(0.0f);
    ui::DimText("Give applist.txt and scan_report.md to any AI assistant (Claude, ChatGPT, Grok, Gemini, or your MCP setup) and ask what each app is, which look suspicious, and what fills the phone "
                "and what is safe to clear. Plain text and Markdown work everywhere.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(6)));
    if (ui::PrimaryButton(ICON_FILE_EXPORT "  Export report", {}, haveData))
        ctx.requestExport = Export_All;
    ImGui::SetItemTooltip("applist.txt + scan_report.md + session log, in a new timestamped folder");
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_FILE_LINES "  Export scan report", {}, haveData))
        ctx.requestExport = Export_ScanReport;
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_COPY "  Copy app list to clipboard", {}, !ctx.apps.Apps().empty()))
        ctx.requestCopyAppList = true;
    ImGui::SetItemTooltip("Same format as applist.txt: paste straight into any AI chat");
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Open reports folder")) {
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        ctx.platform.OpenInExplorer(root);
    }
    {
        Font f(GetFonts().ui, FontSmall);
        ImGui::Dummy(ImVec2(0, S(2)));
        ui::DimText(std::format("{} \xC2\xB7 permissions: {} \xC2\xB7 serial numbers: {}", Narrow(root.wstring()),
            ctx.settings.reportFullPermissions ? "full lists" : "sensitive only", ctx.settings.reportMaskSerial ? "masked" : "shown"));
    }
    ui::EndPanel();
    ImGui::Dummy(ImVec2(0, S(10)));

    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted("Previous exports");
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    if (m_exports.empty()) {
        ui::EmptyState(ICON_FILE_EXPORT, "No exports yet", haveData ? "Click Export report." : "Scan the phone, then export.");
        return;
    }

    const float rowH = S(46);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8), 0));
    if (ImGui::BeginTable("##exports", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, S(170));
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, S(80));
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, S(200));
        ImGui::TableHeadersRow();
        for (const auto& e : m_exports) {
            ImGui::TableNextRow(0, rowH);
            ImGui::PushID(Narrow(e.dir.wstring()).c_str());
            const float ty = (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            const bool latest = e.dir == ctx.lastExportDir;
            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
            ui::ColoredText(latest ? Accent : Text, e.created + (latest ? "  " ICON_CHECK : ""));
            ImGui::TableSetColumnIndex(1);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
            ui::DimText(e.device);
            ImGui::TableSetColumnIndex(2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
            std::string files;
            for (const auto& f : e.files)
                files += (files.empty() ? "" : ", ") + f;
            ui::DimText(files);
            ImGui::TableSetColumnIndex(3);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
            ui::DimText(ui::Bytes(e.bytes));
            ImGui::TableSetColumnIndex(4);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetFrameHeight()) * 0.5f);
            if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Open"))
                ctx.platform.OpenInExplorer(e.dir);
            ImGui::SameLine();
            if (ui::GhostButton(ICON_TRASH "##del"))
                m_pendingDelete = e.dir;
            ImGui::SetItemTooltip("Delete this export folder");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // Confirm deletion of an export folder.
    if (!m_pendingDelete.empty() && !ImGui::IsPopupOpen("Delete export?"))
        ImGui::OpenPopup("Delete export?");
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Delete export?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("Delete %s from the PC? This only removes the report files, nothing on the phone.",
            Narrow(m_pendingDelete.filename().wstring()).c_str());
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ui::DangerButton("Delete")) {
            std::error_code ec;
            // Safety: only ever delete a folder inside the reports root that holds our export.json.
            if (m_pendingDelete.parent_path() == root && std::filesystem::exists(m_pendingDelete / L"export.json", ec)) {
                std::filesystem::remove_all(m_pendingDelete, ec);
                LogInfo("Deleted export {}", Narrow(m_pendingDelete.wstring()));
            }
            m_pendingDelete.clear();
            m_lastList = -100.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_pendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace ps
