#include "screens/BackupPage.h"

#include "adb/AdbClient.h"
#include "app/AppContext.h"
#include "nuke/NukePlan.h"
#include "platform/Platform.h"
#include "scan/FileStore.h"
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

ImVec4 StateColor(RestoreState s)
{
    switch (s) {
    case RestoreState::Missing: return Accent;
    case RestoreState::Present: return Success;
    case RestoreState::Different: return Warning;
    case RestoreState::NoLocal: return Danger;
    }
    return TextDim;
}

bool WillRestore(RestoreState s, RestorePolicy p)
{
    if (s == RestoreState::NoLocal)
        return false;
    return s == RestoreState::Missing || p != RestorePolicy::SkipExisting;
}

} // namespace

void BackupPage::Draw(AppContext& ctx)
{
    if (ctx.filesPreset == "restore") { // dev flag --files=restore
        ctx.filesPreset.clear();
        m_tab = 1;
        m_autoPlan = true;
    } else if (ctx.filesPreset == "archives") {
        ctx.filesPreset.clear();
        m_tab = 2;
    }
    if (ui::Tabs(m_tab, { ICON_DOWNLOAD "  Save queue", ICON_CLOCK_ROTATE "  Restore", ICON_FILE_ZIPPER "  Archives" }))
        m_foldersLoaded = false; // pick up new saves / zips
    ImGui::Dummy(ImVec2(0, S(6)));
    if (m_tab == 0)
        m_queue.Draw(ctx);
    else if (m_tab == 1)
        DrawRestore(ctx);
    else
        DrawArchives(ctx);
}

void BackupPage::StartZip(AppContext& ctx, const std::filesystem::path& folder)
{
    if (m_zipJob && !m_zipJob->Finished())
        return;
    m_zip = std::make_shared<ZipProgress>();
    auto progress = m_zip;
    const bool del = ctx.settings.zipDeleteFolder;
    m_zipJob = ctx.jobs.Submit("Zipping backup", JobPriority::Normal, [progress, folder, del](Job&, std::stop_token st) {
        ZipBackupFolder(folder, del, *progress, st);
    });
}

void BackupPage::DrawArchives(AppContext& ctx)
{
    if (!m_foldersLoaded) {
        m_folders = ListBackupFolders(ctx.settings.BackupRoot());
        m_foldersLoaded = true;
        if (m_folder >= static_cast<int>(m_folders.size()))
            m_folder = -1;
    }
    // Progress / result of the current zip.
    if (m_zip) {
        std::string phase, current, error;
        uint64_t done, total;
        bool finished, ok, removed;
        std::filesystem::path zip;
        {
            auto keep = m_zip;
            std::lock_guard l(keep->mutex);
            phase = keep->phase;
            current = keep->current;
            done = keep->done;
            total = keep->total;
            finished = keep->finished;
            ok = keep->ok;
            error = keep->error;
            zip = keep->zip;
            removed = keep->folderRemoved;
        }
        ui::BeginPanel("##zipprog", ICON_FILE_ZIPPER "  Zip", ImVec2(0, 0));
        if (!finished) {
            {
                Font f(GetFonts().uiStrong, FontBase + 1);
                ImGui::TextUnformatted(std::format("{} {}\xE2\x80\xA6", phase, Narrow(zip.filename().wstring())).c_str());
            }
            ui::DimText(current);
            ui::ProgressBarThin(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImGui::GetContentRegionAvail().x, S(6), Accent);
            ImGui::Dummy(ImVec2(0, S(4)));
            if (ui::SecondaryButton(ICON_XMARK "  Stop") && m_zipJob)
                m_zipJob->Cancel();
        } else {
            if (ok)
                ui::ColoredText(Success, std::format(ICON_CIRCLE_CHECK "  {} written and verified{}", Narrow(zip.filename().wstring()),
                                             removed ? "; the folder was removed" : ""));
            else
                ui::ColoredText(Warning, ICON_WARNING "  Zip not created: " + error + ". Nothing was deleted.");
            ImGui::SameLine();
            if (ok && ui::GhostButton(ICON_EXTERNAL "  Show"))
                ctx.platform.OpenInExplorer(zip.parent_path());
            ImGui::SameLine();
            if (ui::GhostButton("Dismiss")) {
                m_zip.reset();
                m_foldersLoaded = false;
                ctx.requestReloadVerified = true; // zipped files still count as saved
            }
        }
        ui::EndPanel();
        ImGui::Dummy(ImVec2(0, S(6)));
    }
    const bool zipping = m_zipJob && !m_zipJob->Finished();

    ImGui::AlignTextToFramePadding();
    ui::DimText("One file per backup: easy to copy to a drive or the cloud. Every file is read back and checked (CRC + the MD5 from when it was saved).");
    if (ImGui::Checkbox("Delete the folder once its zip is verified", &ctx.settings.zipDeleteFolder))
        ctx.settings.Save();
    ImGui::SameLine();
    if (ui::GhostButton(ICON_ROTATE "##reloadarch"))
        m_foldersLoaded = false;
    ImGui::SetItemTooltip("Reload");
    ImGui::Dummy(ImVec2(0, S(4)));

    if (m_folders.empty()) {
        ui::EmptyState(ICON_FILE_ZIPPER, "No backups yet", "Save files from the phone first; each backup folder can then be zipped here.");
        return;
    }
    const float rowH = S(48);
    ImGui::BeginChild("##archives", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < m_folders.size(); ++i) {
        const auto& f = m_folders[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float lh = ImGui::GetTextLineHeight();
        const float y0 = p.y + (rowH - lh * 2) * 0.5f;
        dl->AddText(ImVec2(p.x + S(12), p.y + (rowH - lh) * 0.5f), U32(f.zip ? Info : TextDim), f.zip ? ICON_FILE_ZIPPER : ICON_FOLDER_OPEN);
        const std::string name = Narrow(f.folder.filename().wstring());
        dl->AddText(ImVec2(p.x + S(40), y0), U32(Text), name.c_str());
        {
            Font sf(GetFonts().ui, FontSmall);
            std::string meta = std::format("{}  \xC2\xB7  {} files  \xC2\xB7  {}", f.model.empty() ? MaskSerial(f.serial) : f.model, ui::Count(f.files), ui::Bytes(f.bytes));
            if (f.zip) {
                std::error_code ec;
                meta += std::format("  \xC2\xB7  zip {}", ui::Bytes(std::filesystem::file_size(f.folder, ec)));
            }
            dl->AddText(ImVec2(p.x + S(40), y0 + lh), U32(TextDim), meta.c_str());
        }
        // Right side.
        bool hasZip = false;
        if (!f.zip)
            for (const auto& o : m_folders)
                hasZip |= o.zip && o.folder.stem() == f.folder.filename();
        const char* btn = f.zip ? ICON_EXTERNAL "  Show" : ICON_FILE_ZIPPER "  Zip";
        const float bw = ui::TextWidth(btn) + S(28);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - S(12), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
        if (f.zip) {
            if (ui::SecondaryButton(btn, ImVec2(bw, 0)))
                ctx.platform.OpenInExplorer(f.folder.parent_path());
        } else {
            if (hasZip) {
                const float pw = ui::TextWidth("ZIPPED") + S(40);
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - pw - S(12), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f + S(4)));
                ui::Pill("ZIPPED", Success);
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - S(12), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
            }
            if (ui::PrimaryButton(btn, ImVec2(bw, 0), !zipping))
                StartZip(ctx, f.folder);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
        ImGui::Dummy(ImVec2(w, 0));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void BackupPage::StartPlan(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ActiveConnected() || m_folder < 0 || m_folder >= static_cast<int>(m_folders.size()))
        return;
    if (m_planJob)
        m_planJob->Cancel();
    m_plan = std::make_shared<PlanShared>();
    m_plan->running = true;
    m_planned = true;
    m_items.clear();
    auto shared = m_plan;
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial;
    const auto folder = m_folders[m_folder].folder;
    m_planJob = ctx.jobs.Submit("Comparing backup with the phone", JobPriority::Normal, [=](Job&, std::stop_token st) {
        // Ask the phone itself: the file scan may not have found the SD card yet.
        auto items = PlanRestore(*adb, serial, folder, PhoneVolumeRoots(*adb, serial, st), st);
        std::lock_guard l(shared->mutex);
        shared->items = std::move(items);
        shared->running = false;
        shared->done = true;
    });
}

void BackupPage::StartRestore(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    const auto policy = static_cast<RestorePolicy>(m_policy);
    std::vector<RestoreItem> todo;
    for (const auto& it : m_items)
        if (WillRestore(it.state, policy))
            todo.push_back(it);
    if (todo.empty())
        return;
    m_progress = std::make_shared<RestoreProgress>();
    auto progress = m_progress;
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial;
    m_runJob = ctx.jobs.Submit("Restoring to the phone", JobPriority::Normal, [=, todo = std::move(todo)](Job&, std::stop_token st) {
        RunRestore(*adb, serial, todo, policy, *progress, st);
    });
}

void BackupPage::DrawRestore(AppContext& ctx)
{
    if (ctx.activeSerial != m_serial) {
        m_serial = ctx.activeSerial;
        m_planned = false;
        m_items.clear();
    }
    if (!m_foldersLoaded) {
        m_folders = ListBackupFolders(ctx.settings.BackupRoot());
        m_foldersLoaded = true;
        if (m_folder >= static_cast<int>(m_folders.size()))
            m_folder = -1;
        // Default: newest backup of this phone, else the newest of any phone.
        if (m_folder < 0)
            for (int i = 0; i < static_cast<int>(m_folders.size()); ++i)
                if (m_folders[i].serial == ctx.DeviceId()) {
                    m_folder = i;
                    break;
                }
        if (m_folder < 0 && !m_folders.empty())
            m_folder = 0;
    }
    if (m_autoPlan && ctx.ActiveConnected() && m_folder >= 0) {
        m_autoPlan = false;
        StartPlan(ctx); // read-only: only stats the phone
    }
    if (m_progress) {
        DrawRestoreRunning(ctx);
        return;
    }
    {
        auto shared = m_plan;
        std::lock_guard l(shared->mutex);
        if (shared->done) {
            shared->done = false;
            m_items = std::move(shared->items);
        }
    }
    if (m_folders.empty()) {
        ui::EmptyState(ICON_CLOCK_ROTATE, "No backups yet", "Files you save with PhoneSweep can be put back on this (or another) phone from here.");
        return;
    }

    // Folder picker.
    auto describe = [](const BackupFolderInfo& f) {
        return std::format("{}  \xC2\xB7  {}  \xC2\xB7  {} files, {}", Narrow(f.folder.filename().wstring()), f.model.empty() ? f.serial : f.model,
            ui::Count(f.files), ui::Bytes(f.bytes)) + (f.zip ? "  \xC2\xB7  zip" : "");
    };
    ImGui::AlignTextToFramePadding();
    ui::DimText("Backup");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(460));
    if (ImGui::BeginCombo("##bfolder", m_folder >= 0 ? describe(m_folders[m_folder]).c_str() : "Pick a backup")) {
        for (int i = 0; i < static_cast<int>(m_folders.size()); ++i)
            if (ImGui::Selectable(describe(m_folders[i]).c_str(), m_folder == i) && m_folder != i) {
                m_folder = i;
                m_planned = false;
                m_items.clear();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui::GhostButton(ICON_ROTATE "##reloadfolders"))
        m_foldersLoaded = false;
    ImGui::SetItemTooltip("Reload the backup list");
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_EXTERNAL "  Open", {}, m_folder >= 0))
        ctx.platform.OpenInExplorer(m_folders[m_folder].folder);
    ImGui::SameLine();
    bool planning;
    {
        auto shared = m_plan;
        std::lock_guard l(shared->mutex);
        planning = shared->running;
    }
    if (ui::PrimaryButton(m_planned ? ICON_ROTATE "  Check again" : ICON_SEARCH "  Compare with the phone", {},
            !planning && m_folder >= 0 && ctx.ActiveConnected()))
        StartPlan(ctx);

    if (planning) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ui::DimText("Checking which files are already on the phone\xE2\x80\xA6");
        ui::SkeletonRows(6, S(36));
        return;
    }
    if (!m_planned) {
        ui::EmptyState(ICON_CLOCK_ROTATE, "Restore a backup",
            "Pick a backup and compare it with the phone. Only files that are missing are put back unless you choose otherwise.");
        return;
    }

    // Summary.
    size_t counts[4] = {};
    for (const auto& it : m_items)
        ++counts[static_cast<int>(it.state)];
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::AlignTextToFramePadding();
    for (int s = 0; s < 4; ++s) {
        if (!counts[s])
            continue;
        ui::Pill(std::format("{} {}", ui::Count(counts[s]), ToString(static_cast<RestoreState>(s))), StateColor(static_cast<RestoreState>(s)));
        ImGui::SameLine(0, S(8));
    }
    ImGui::NewLine();

    // Policy.
    const auto mode = static_cast<ProtectorMode>(std::clamp(ctx.settings.nukeMode, 0, 1));
    if (mode == ProtectorMode::Prompt && m_policy == static_cast<int>(RestorePolicy::Overwrite))
        m_policy = static_cast<int>(RestorePolicy::SkipExisting);
    ImGui::Dummy(ImVec2(0, S(4)));
    auto radio = [&](const char* label, RestorePolicy p) {
        if (ui::Radio(label, m_policy == static_cast<int>(p)))
            m_policy = static_cast<int>(p);
    };
    radio("Only files missing on the phone", RestorePolicy::SkipExisting);
    ImGui::SameLine(0, S(16));
    radio("Keep both (adds \" (restored)\")", RestorePolicy::KeepBoth);
    ImGui::SameLine(0, S(16));
    ImGui::BeginDisabled(mode == ProtectorMode::Prompt);
    radio("Overwrite same names", RestorePolicy::Overwrite);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(mode == ProtectorMode::Prompt ? "Nuke prompt blocks this: overwriting replaces whatever is on the phone now. Use Keep both, or "
                                                          "switch to Nuke free in Settings."
                                                        : "Replaces the phone's file with the backup copy.");

    const auto policy = static_cast<RestorePolicy>(m_policy);
    size_t n = 0;
    uint64_t bytes = 0;
    for (const auto& it : m_items)
        if (WillRestore(it.state, policy)) {
            ++n;
            bytes += it.entry.size;
        }

    // Item list.
    const float footer = S(52);
    const float rowH = S(36);
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::BeginChild("##ritems", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_items.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto& it = m_items[r];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const bool go = WillRestore(it.state, policy);
            const float lh = ImGui::GetTextLineHeight();
            const std::string name = it.target.substr(it.target.rfind('/') + 1);
            std::string folder = it.target.substr(0, it.target.rfind('/'));
            if (folder.starts_with("/storage/emulated/0/"))
                folder = folder.substr(20);
            else if (folder.starts_with("/storage/") && folder.find('/', 9) != std::string::npos)
                folder = "SD card \xC2\xB7 " + folder.substr(folder.find('/', 9) + 1);
            dl->AddText(ImVec2(p.x + S(10), p.y + (rowH - lh) * 0.5f), U32(go ? Text : TextDim), name.c_str());
            const float nx = p.x + S(10) + ImGui::CalcTextSize(name.c_str()).x + S(12); // measured in the name's font
            {
                Font f(GetFonts().ui, FontSmall);
                dl->AddText(ImVec2(nx, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), U32(TextDim),
                    std::format("{}  \xC2\xB7  {}", folder, ui::Bytes(it.entry.size)).c_str());
                const char* tag = ToString(it.state);
                const ImVec4 col = StateColor(it.state);
                const ImVec2 ts = ImGui::CalcTextSize(tag);
                const ImVec2 b1(p.x + w - S(12), p.y + (rowH + ts.y) * 0.5f + S(2));
                const ImVec2 b0(b1.x - ts.x - S(14), b1.y - ts.y - S(4));
                dl->AddRectFilled(b0, b1, U32(col, 0.16f), (b1.y - b0.y) * 0.5f);
                dl->AddText(ImVec2(b0.x + S(7), b0.y + S(2)), U32(col), tag);
            }
            ImGui::Dummy(ImVec2(w, rowH));
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, S(6)));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(std::format("Will restore {} files \xC2\xB7 {}", ui::Count(n), ui::Bytes(bytes)).c_str());
    }
    ImGui::SameLine();
    const std::string label = std::format(ICON_UPLOAD "  Restore {} files", ui::Count(n));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ui::TextWidth(label) - S(30)));
    const bool enabled = n > 0 && ctx.ActiveConnected() && !ctx.deleteRunning;
    if (policy == RestorePolicy::Overwrite ? ui::DangerButton(label.c_str(), {}, enabled) : ui::PrimaryButton(label.c_str(), {}, enabled)) {
        if (policy == RestorePolicy::Overwrite)
            m_confirmOpen = true;
        else
            StartRestore(ctx);
    }

    if (m_confirmOpen) {
        ImGui::OpenPopup("Overwrite files on the phone?");
        m_confirmOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(460), 0));
    if (ImGui::BeginPopupModal("Overwrite files on the phone?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s files on the phone will be replaced by the backup copy. The phone's current version is lost.",
            ui::Count(counts[static_cast<int>(RestoreState::Different)] + counts[static_cast<int>(RestoreState::Present)]).c_str());
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ui::DangerButton(ICON_UPLOAD "  Overwrite")) {
            StartRestore(ctx);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void BackupPage::DrawRestoreRunning(AppContext& ctx)
{
    size_t done, total, restored, skipped, failed;
    uint64_t bytes;
    std::string current;
    std::vector<std::string> errors;
    bool finished;
    {
        auto keep = m_progress;
        std::lock_guard l(keep->mutex);
        done = keep->done;
        total = keep->total;
        restored = keep->restored;
        skipped = keep->skipped;
        failed = keep->failed;
        bytes = keep->bytes;
        current = keep->current;
        errors = keep->errors;
        finished = keep->finished;
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    {
        Font f(GetFonts().uiStrong, FontBase + 2);
        ImGui::TextUnformatted(finished ? std::format(ICON_CIRCLE_CHECK "  Restored {} files ({})", ui::Count(restored), ui::Bytes(bytes)).c_str()
                                        : std::format("Restoring {} of {}", std::min(done + 1, total), total).c_str());
    }
    ui::DimText(finished ? std::format("{} skipped \xC2\xB7 {} failed", skipped, failed) : current);
    ImGui::Dummy(ImVec2(0, S(6)));
    ui::ProgressBarThin(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImGui::GetContentRegionAvail().x, S(6), Accent);
    for (const auto& e : errors)
        ui::ColoredText(Warning, ICON_WARNING "  " + e);
    ImGui::Dummy(ImVec2(0, S(10)));
    if (!finished) {
        if (ui::SecondaryButton(ICON_XMARK "  Stop") && m_runJob)
            m_runJob->Cancel();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ui::DimText("Each file is uploaded under a temporary name and only renamed into place once its MD5 matches.");
    } else if (ui::PrimaryButton(ICON_CHECK "  Done \xE2\x80\x94 rescan the phone")) {
        m_progress.reset();
        m_planned = false;
        m_items.clear();
        ctx.requestScan = true;
    }
}

} // namespace ps
