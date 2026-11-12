#include "screens/FileDeleter.h"

#include "app/AppContext.h"
#include "nuke/NukePlan.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {
constexpr const char* kPopup = "Delete permanently?";
}

void FileDeleter::Ask(AppContext& ctx, const std::vector<uint32_t>& indices)
{
    m_paths.clear();
    m_unsaved.clear();
    m_bytes = 0;
    m_protected = 0;
    const auto mode = static_cast<ProtectorMode>(std::clamp(ctx.settings.nukeMode, 0, 1));
    const auto& files = ctx.files.Files();
    for (uint32_t i : indices) {
        if (i >= files.size())
            continue;
        const FileEntry& e = files[i];
        NukeFileFacts f;
        f.keep = e.flags & FileFlag_Keep;
        f.verified = ctx.verified.IsVerified(e.path, e.size, e.mtime);
        f.isProtected = e.IsProtected();
        f.userIncludedProtected = true; // picked by hand = explicitly chosen
        switch (DecideNuke(f, mode)) {
        case NukeDecision::Delete:
            m_paths.push_back(e.path);
            m_bytes += e.Disk();
            break;
        case NukeDecision::Protected: ++m_protected; break;
        default: m_unsaved.push_back(i); break; // NotSaved / KeepUnsaved
        }
    }
    m_open = true;
}

void FileDeleter::Draw(AppContext& ctx)
{
    if (m_open) {
        ImGui::OpenPopup(kPopup);
        m_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(500), 0));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;
    const bool prompt = ctx.settings.nukeMode == 0;
    if (!m_paths.empty()) {
        ImGui::TextWrapped("Delete %s %s (%s) from the phone? This can't be undone.", ui::Count(m_paths.size()).c_str(),
            m_paths.size() == 1 ? "file" : "files", ui::Bytes(m_bytes).c_str());
        if (m_paths.size() <= 6) {
            Font f(GetFonts().ui, FontSmall);
            for (const auto& p : m_paths)
                ui::ColoredText(Danger, std::string(ICON_TRASH "  ") + p.substr(p.rfind('/') + 1));
        }
    } else {
        ImGui::TextWrapped("Nothing can be deleted.");
    }
    if (!m_unsaved.empty() || m_protected) {
        ImGui::Dummy(ImVec2(0, S(4)));
        Font f(GetFonts().ui, FontSmall);
        if (!m_unsaved.empty())
            ui::ColoredText(Accent, std::format(ICON_SHIELD "  {} {} not saved on the PC yet, so {} keeps {}.", ui::Count(m_unsaved.size()),
                                        m_unsaved.size() == 1 ? "is" : "are", prompt ? "Nuke prompt" : "KEEP", m_unsaved.size() == 1 ? "it" : "them"));
        if (m_protected)
            ui::ColoredText(Warning, std::format(ICON_LOCK "  {} in protected folders: only Nuke free can delete those.", ui::Count(m_protected)));
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    if (!m_paths.empty() && ui::DangerButton(std::format(ICON_TRASH "  Delete {}", m_paths.size() == 1 ? std::string("it") : ui::Count(m_paths.size())).c_str(),
            {}, ctx.ActiveConnected() && !ctx.deleteRunning)) {
        DeleteRequest req;
        req.paths = std::move(m_paths);
        req.label = req.paths.size() == 1 ? "file" : "files";
        ctx.requestDelete = std::move(req);
        m_paths.clear();
        ImGui::CloseCurrentPopup();
    }
    if (!m_unsaved.empty()) {
        if (!m_paths.empty())
            ImGui::SameLine();
        if (ui::SecondaryButton(std::format(ICON_FLOPPY "  Save the {} first", ui::Count(m_unsaved.size())).c_str())) {
            ctx.requestSaveFiles.insert(ctx.requestSaveFiles.end(), m_unsaved.begin(), m_unsaved.end());
            ui::Toast("Saving them to the PC; delete again once they're saved", ui::ToastKind::Info);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace ps
