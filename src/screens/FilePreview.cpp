#include "screens/FilePreview.h"

#include "app/AppContext.h"
#include "platform/Platform.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
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
constexpr const char* kPopup = "##filepreview";
}

void FilePreview::Open(const std::string& path)
{
    m_path = path;
    m_justOpened = true;
}

void FilePreview::Close()
{
    m_path.clear();
    m_kit.StopAudio();
}

void FilePreview::Draw(AppContext& ctx, const std::vector<uint32_t>& view)
{
    m_kit.Pump(ctx); // also serves the right-click "Open on PC" while closed
    const auto& files = ctx.files.Files();

    // Find the file again (indices move when files are deleted or rescanned).
    int64_t index = -1;
    if (!m_path.empty()) {
        if (m_pos < view.size() && files[view[m_pos]].path == m_path) {
            index = view[m_pos];
        } else {
            for (size_t i = 0; i < view.size(); ++i)
                if (files[view[i]].path == m_path) {
                    m_pos = i;
                    index = view[i];
                    break;
                }
            if (index < 0 && !view.empty() && !m_justOpened) {
                // Gone (deleted): show the file that took its place.
                m_pos = std::min(m_pos, view.size() - 1);
                index = view[m_pos];
                m_path = files[static_cast<size_t>(index)].path;
            }
        }
    }
    if (index < 0) {
        if (ImGui::BeginPopupModal(kPopup)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!m_path.empty())
            Close();
        return;
    }
    if (m_justOpened) {
        ImGui::OpenPopup(kPopup);
        m_justOpened = false;
    }

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

    const FileEntry& e = files[static_cast<size_t>(index)];
    size_t go = m_pos;
    bool close = false;

    // Header: icon + name, position, prev / next / close.
    {
        Font f(GetFonts().uiStrong, FontH2 - 4);
        ui::ColoredText(ui::CategoryColor(static_cast<uint8_t>(e.category)), ui::CategoryIcon(static_cast<uint8_t>(e.category)));
        ImGui::SameLine(0, S(10));
        const float maxW = ImGui::GetContentRegionAvail().x - S(220);
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + maxW, ImGui::GetCursorScreenPos().y + S(40)), true);
        ImGui::TextUnformatted(e.Name().data(), e.Name().data() + e.Name().size());
        ImGui::PopClipRect();
    }
    {
        const float bw = S(34);
        const std::string pos = std::format("{} / {}", ui::Count(m_pos + 1), ui::Count(view.size()));
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 3 - S(28) - ui::TextWidth(pos));
        ImGui::AlignTextToFramePadding();
        ui::DimText(pos);
        ImGui::SameLine(0, S(10));
        if (ui::SecondaryButton(ICON_CHEVRON_LEFT "##prev", ImVec2(bw, 0), m_pos > 0))
            go = m_pos - 1;
        ImGui::SetItemTooltip("Previous file (\xE2\x86\x90)");
        ImGui::SameLine(0, S(4));
        if (ui::SecondaryButton(ICON_CHEVRON_RIGHT "##next", ImVec2(bw, 0), m_pos + 1 < view.size()))
            go = m_pos + 1;
        ImGui::SetItemTooltip("Next file (\xE2\x86\x92)");
        ImGui::SameLine(0, S(12));
        if (ui::GhostButton(ICON_XMARK "##closepv", ImVec2(bw, 0)))
            close = true;
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    // Body: preview left, details + actions right.
    const float sideW = S(300);
    const float bodyH = ImGui::GetContentRegionAvail().y;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + ImGui::GetContentRegionAvail().x - sideW - S(16), a.y + bodyH);
    m_kit.Draw(ctx, e, a, b, true);

    ImGui::SetCursorScreenPos(ImVec2(b.x + S(16), a.y));
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH));
    auto meta = [&](const char* label, const std::string& value) {
        ui::DimText(label);
        ImGui::SameLine(S(84));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    };
    meta("Size", ui::Bytes(e.size));
    meta("Modified", ui::Date(e.mtime));
    meta("Type", e.mime.empty() ? std::string(CategoryName(e.category)) : e.mime);
    if (e.source != Source::Other)
        meta("Source", SourceName(e.source));
    meta("Folder", FolderCaption(e.path));
    ImGui::Dummy(ImVec2(0, S(4)));
    const bool verified = ctx.verified.IsVerified(e.path, e.size, e.mtime);
    const bool keep = e.flags & FileFlag_Keep;
    if (verified)
        ui::Pill(ICON_CHECK " SAVED ON PC", Success);
    if (keep) {
        if (verified)
            ImGui::SameLine(0, S(6));
        ui::Pill("KEEP", Accent);
    }
    if (e.IsProtected())
        ui::Pill(std::format("PROTECTED \xC2\xB7 {}", ProtectionName(e.protection)), Warning);

    ImGui::Dummy(ImVec2(0, S(12)));
    const float w = ImGui::GetContentRegionAvail().x;
    if (PreviewKit::CanOpenOnPc(e)) {
        if (ui::SecondaryButton(e.category == Category::Videos ? ICON_PLAY "  Play on PC" : ICON_EXTERNAL "  Open on PC", ImVec2(w, 0)))
            m_kit.OpenOnPc(ctx, e);
        ImGui::SetItemTooltip("Opens it in the default Windows app.");
    }
    if (verified) {
        if (const auto local = ctx.verified.LocalFile(e.path); local && ui::SecondaryButton(ICON_FOLDER_OPEN "  Show saved copy", ImVec2(w, 0)))
            ctx.platform.OpenInExplorer(local->parent_path());
    } else if (ui::SecondaryButton(ICON_FLOPPY "  Save to PC", ImVec2(w, 0))) {
        ctx.requestSaveFiles.push_back(static_cast<uint32_t>(index));
        ui::Toast("Saving to the PC (Save queue shows progress)", ui::ToastKind::Info);
    }
    if (keep ? ui::SecondaryButton("Unmark keep", ImVec2(w, 0)) : ui::SecondaryButton(ICON_CHECK "  Mark as keep", ImVec2(w, 0)))
        ctx.files.SetKeep(static_cast<size_t>(index), !keep);
    if (ui::GhostButton(ICON_COPY "  Copy path", ImVec2(w, 0))) {
        ctx.platform.SetClipboardText(e.path);
        ui::Toast("Copied to clipboard");
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    if (ui::DangerButton(ICON_TRASH "  Delete permanently", ImVec2(w, 0), ctx.ActiveConnected() && !ctx.deleteRunning))
        m_deleter.Ask(ctx, { static_cast<uint32_t>(index) });
    {
        Font f(GetFonts().ui, FontSmall);
        ImGui::Dummy(ImVec2(0, S(10)));
        ui::DimText("\xE2\x86\x90 \xE2\x86\x92  browse   \xC2\xB7   Esc  close");
        if (e.category == Category::Images)
            ui::DimText("Scroll to zoom, drag to move, double-click to fit");
    }
    ImGui::EndChild();

    // Keys (not while a confirmation is up).
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && m_pos > 0)
            go = m_pos - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && m_pos + 1 < view.size())
            go = m_pos + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
    }
    if (close) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        Close();
        return;
    }
    if (go != m_pos && go < view.size()) {
        m_kit.StopAudio();
        m_pos = go;
        m_path = files[view[go]].path;
    }
    m_deleter.Draw(ctx);
    ImGui::EndPopup();
}

} // namespace ps
