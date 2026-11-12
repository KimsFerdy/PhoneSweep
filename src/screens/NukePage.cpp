#include "screens/NukePage.h"

#include "app/AppContext.h"
#include "app/Settings.h"
#include "platform/Platform.h"
#include "scan/AppSession.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
#include "transfer/Puller.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>

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

// Row with an icon, bold headline, dim detail and optional trailing buttons area.
void PlanRow(const char* icon, const ImVec4& iconCol, const std::string& headline, const std::string& detail)
{
    ImGui::AlignTextToFramePadding();
    ui::ColoredText(iconCol, icon);
    ImGui::SameLine(0, S(12));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::TextUnformatted(headline.c_str());
    }
    if (!detail.empty()) {
        ImGui::SameLine(0, S(10));
        ui::DimText(detail);
    }
}

void Notice(const ImVec4& col, const char* icon, const std::string& text)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const std::string line = std::string(icon) + "  " + text;
    const ImVec2 ts = ImGui::CalcTextSize(line.c_str(), nullptr, false, w - S(24));
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + ts.y + S(16)), U32(col, 0.10f), S(RoundButton));
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + S(8)));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - S(24));
    ui::ColoredText(Mix(col, Text, 0.35f), line);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ts.y + S(16)));
    ImGui::Dummy(ImVec2(w, S(4)));
}

std::string NowStamp()
{
    const auto now = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return std::format("{:%Y%m%d_%H%M%S}", now);
}

} // namespace

void NukePage::RebuildPlan(AppContext& ctx)
{
    const auto mode = static_cast<ProtectorMode>(std::clamp(ctx.settings.nukeMode, 0, 1));
    // Cheap cache key: anything that changes the plan.
    uint64_t key = ctx.files.Version() * 1000003ull ^ ctx.verified.Version() * 9176ull ^ ctx.apps.Version() * 31ull ^ m_ovVersion * 7919ull
        ^ static_cast<uint64_t>(mode);
    if (key == m_planKey)
        return;
    m_planKey = key;
    m_plan = BuildNukePlan(ctx.files, ctx.verified, ctx.apps.Apps(), ctx.apps.UninstallSet(), m_ov, mode);
}

void NukePage::Draw(AppContext& ctx)
{
    if (ctx.activeSerial != m_serial) {
        m_serial = ctx.activeSerial;
        m_ov = {};
        ++m_ovVersion;
        m_progress.reset();
        m_dryRunFingerprint = 0;
        m_confirm[0] = 0;
    }

    // Danger styling: thin red line along the top of the page.
    {
        const ImVec2 p = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y + S(3)), U32(Danger));
    }

    if (m_progress) {
        bool finished;
        {
            std::lock_guard l(m_progress->mutex);
            finished = m_progress->finished;
        }
        if (finished) {
            if (!m_finishedHandled) {
                Finish(ctx);
                m_finishedHandled = true;
            }
            DrawResult(ctx);
        } else {
            DrawRunning(ctx);
        }
        return;
    }

    if (ctx.files.Files().empty() && ctx.apps.Apps().empty()) {
        ui::EmptyState(ICON_FIRE, "Nothing to plan yet", "Scan the phone first (Overview \xE2\x86\x92 Rescan).");
        return;
    }
    RebuildPlan(ctx);
    ImGui::BeginChild("##nukeplan", ImVec2(0, ImGui::GetContentRegionAvail().y - S(150)));
    DrawPlan(ctx);
    ImGui::EndChild();
    DrawConfirm(ctx);
}

void NukePage::DrawPlan(AppContext& ctx)
{
    const bool prompt = m_plan.mode == ProtectorMode::Prompt;

    // Protector mode banner.
    {
        ImGui::AlignTextToFramePadding();
        ui::ColoredText(prompt ? Accent : Danger, prompt ? ICON_SHIELD : ICON_FIRE);
        ImGui::SameLine(0, S(8));
        {
            Font f(GetFonts().uiStrong, FontBase);
            ImGui::TextUnformatted(prompt ? "Nuke prompt" : "Nuke free");
        }
        ImGui::SameLine(0, S(8));
        ui::DimText(prompt ? "only files saved + verified on the PC are deleted" : "everything in the plan is deleted forever, saved or not");
        ImGui::SameLine();
        if (ui::GhostButton("Change\xE2\x80\xA6##mode")) {
            ctx.showSettings = true;
            ctx.settingsTab = 5;
        }
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    ui::BeginPanel("##plan", ICON_FIRE "  Plan", ImVec2(0, 0)); // height 0 = grow with content

    // Delete.
    PlanRow(ICON_TRASH, Danger, std::format("Delete {} files", ui::Count(m_plan.del.files.size())), ui::Bytes(m_plan.del.bytes));
    ImGui::SameLine();
    if (ui::GhostButton(m_showFiles ? ICON_CHEVRON_DOWN "  Hide list##f" : ICON_CHEVRON_RIGHT "  Review list##f"))
        m_showFiles = !m_showFiles;
    if (m_showFiles)
        DrawFileReview(ctx, m_plan.del, false);

    // Uninstall.
    PlanRow(ICON_CUBES, Danger, std::format("Uninstall {} apps", m_plan.uninstall.size()),
        m_plan.uninstall.empty() ? std::string("flag apps in Apps \xE2\x86\x92 Flag for uninstall")
                                 : (ctx.settings.backupApksBeforeUninstall ? std::string("APKs are backed up first") : std::string("no APK backup")));
    if (!m_plan.uninstall.empty()) {
        ImGui::SameLine();
        if (ui::GhostButton(m_showApps ? ICON_CHEVRON_DOWN "  Hide list##a" : ICON_CHEVRON_RIGHT "  Review list##a"))
            m_showApps = !m_showApps;
        if (m_showApps)
            DrawAppReview(ctx);
    }

    // Caches.
    PlanRow(ICON_BROOM, Warning, "Then clear all app caches", "pm trim-caches, then empty folders and the gallery are tidied");
    ImGui::Dummy(ImVec2(0, S(8)));

    // ---- Why other files stay ----
    if (!m_plan.keepUnsaved.files.empty()) {
        if (prompt)
            Notice(Danger, ICON_BAN,
                std::format("Blocker: {} files ({}) are marked KEEP but aren't saved on the PC yet. Save them, or unmark KEEP, before nuking.",
                    ui::Count(m_plan.keepUnsaved.files.size()), ui::Bytes(m_plan.keepUnsaved.bytes)));
        else
            Notice(Warning, ICON_INFO,
                std::format("{} KEEP files ({}) aren't saved yet, so they stay on the phone.", ui::Count(m_plan.keepUnsaved.files.size()),
                    ui::Bytes(m_plan.keepUnsaved.bytes)));
        if (ui::PrimaryButton(ICON_FLOPPY "  Save KEEP files now")) {
            ctx.requestSaveKept = true;
            ctx.page = Page::SaveQueue;
        }
        ImGui::Dummy(ImVec2(0, S(6)));
    }
    if (!m_plan.notSaved.files.empty()) {
        Notice(Accent, ICON_SHIELD,
            std::format("{} files ({}) aren't saved on the PC, so Nuke prompt leaves them on the phone. Save them first, or switch to Nuke free "
                        "to delete them without saving.",
                ui::Count(m_plan.notSaved.files.size()), ui::Bytes(m_plan.notSaved.bytes)));
        if (ui::SecondaryButton(std::format(ICON_FLOPPY "  Save all {} now ({})", ui::Count(m_plan.notSaved.files.size()), ui::Bytes(m_plan.notSaved.bytes)).c_str())) {
            ctx.requestSaveFiles.insert(ctx.requestSaveFiles.end(), m_plan.notSaved.files.begin(), m_plan.notSaved.files.end());
            ctx.page = Page::SaveQueue;
        }
        ImGui::Dummy(ImVec2(0, S(6)));
    }
    {
        Notice(Success, ICON_LOCK,
            std::format("Protected: Android/data, Android/obb, hidden folders ({} files, {}){}", ui::Count(m_plan.protectedFiles.files.size()),
                ui::Bytes(m_plan.protectedFiles.bytes), prompt ? ", untouched" : ", untouched unless you tick them below"));
        if (!prompt && (!m_plan.protectedFiles.files.empty() || !m_ov.includedProtectedPaths.empty())) {
            if (ui::GhostButton(m_showProtected ? ICON_CHEVRON_DOWN "  Hide protected##p" : ICON_CHEVRON_RIGHT "  Review protected files##p"))
                m_showProtected = !m_showProtected;
            if (m_showProtected)
                DrawFileReview(ctx, m_plan.protectedFiles, true);
        }
    }
    if (!m_plan.excluded.files.empty()) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ui::DimText(std::format("{} files unticked by you ({})", ui::Count(m_plan.excluded.files.size()), ui::Bytes(m_plan.excluded.bytes)));
        ImGui::SameLine();
        if (ui::GhostButton("Tick all again")) {
            m_ov.excludedPaths.clear();
            ++m_ovVersion;
        }
    }
    ui::EndPanel();
}

void NukePage::DrawFileReview(AppContext& ctx, const NukeGroup& group, bool protectedList)
{
    const auto& files = ctx.files.Files();
    ImGui::Indent(S(30));
    ImGui::SetNextItemWidth(S(280));
    ImGui::InputTextWithHint(protectedList ? "##psearch" : "##fsearch", ICON_SEARCH "  Filter", m_search, sizeof(m_search));
    ImGui::SameLine();
    if (protectedList) {
        if (ui::GhostButton("Tick all shown##p")) {
            for (uint32_t i : group.files)
                if (IContains(files[i].path, m_search))
                    m_ov.includedProtectedPaths.insert(files[i].path);
            ++m_ovVersion;
        }
    } else if (ui::GhostButton("Untick all shown##f")) {
        for (uint32_t i : group.files)
            if (IContains(files[i].path, m_search))
                m_ov.excludedPaths.insert(files[i].path);
        ++m_ovVersion;
    }

    // Filtered rows.
    std::vector<uint32_t> rows;
    rows.reserve(group.files.size());
    for (uint32_t i : group.files)
        if (IContains(files[i].path, m_search))
            rows.push_back(i);
    // In the protected list, also show the ones already ticked (they're in the delete group).
    if (protectedList)
        for (uint32_t i : m_plan.del.files)
            if (files[i].IsProtected() && IContains(files[i].path, m_search))
                rows.push_back(i);

    const float rowH = ImGui::GetFrameHeight();
    const float h = std::min(S(320), rowH * static_cast<float>(rows.size()) + S(8));
    ImGui::BeginChild(protectedList ? "##prot" : "##del", ImVec2(0, std::max(h, rowH)), ImGuiChildFlags_Borders);
    Font f(GetFonts().mono, FontSmall);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const FileEntry& e = files[rows[r]];
            bool ticked = protectedList ? m_ov.includedProtectedPaths.contains(e.path) : true;
            ImGui::PushID(static_cast<int>(rows[r]));
            if (ImGui::Checkbox("##t", &ticked)) {
                if (protectedList) {
                    if (ticked)
                        m_ov.includedProtectedPaths.insert(e.path);
                    else
                        m_ov.includedProtectedPaths.erase(e.path);
                } else {
                    m_ov.excludedPaths.insert(e.path);
                }
                ++m_ovVersion;
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ui::DimText(ui::Bytes(e.size));
            ImGui::SameLine(S(110));
            ImGui::TextUnformatted(e.path.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::Unindent(S(30));
    ImGui::Dummy(ImVec2(0, S(4)));
}

void NukePage::DrawAppReview(AppContext& ctx)
{
    ImGui::Indent(S(30));
    for (const auto& pkg : m_plan.uninstall) {
        bool ticked = true;
        ImGui::PushID(pkg.c_str());
        if (ImGui::Checkbox("##app", &ticked) && !ticked) {
            m_ov.excludedApps.insert(pkg);
            ++m_ovVersion;
        }
        ImGui::SameLine();
        std::string label = pkg;
        for (const auto& a : ctx.apps.Apps())
            if (a.package == pkg && !a.label.empty())
                label = std::format("{}  ({})", a.label, pkg);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopID();
    }
    if (!m_ov.excludedApps.empty()) {
        if (ui::GhostButton("Tick all apps again")) {
            m_ov.excludedApps.clear();
            ++m_ovVersion;
        }
    }
    ImGui::Unindent(S(30));
}

void NukePage::DryRun(AppContext& ctx)
{
    const auto dir = Settings::DataDir() / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / Widen(std::format("nuke_plan_{}.txt", NowStamp()));
    std::ofstream out(file, std::ios::binary);
    out << FormatNukePlan(m_plan, ctx.files, ctx.activeInfo.displayName);
    if (!out) {
        ui::Toast("Couldn't write the plan file", ui::ToastKind::Error);
        return;
    }
    m_lastPlanFile = file;
    m_dryRunFingerprint = m_plan.Fingerprint();
    LogInfo("Nuke dry run written: {} ({} files, {} apps)", Narrow(file.wstring()), m_plan.del.files.size(), m_plan.uninstall.size());
    ui::Toast("Dry run written to the log folder");
}

void NukePage::DrawConfirm(AppContext& ctx)
{
    ImGui::Dummy(ImVec2(0, S(6)));
    ui::HLine(0.8f);
    ImGui::Dummy(ImVec2(0, S(6)));

    if (ui::SecondaryButton(ICON_FILE_LINES "  Dry run \xE2\x86\x92 write plan to log", {}, !m_plan.Empty()))
        DryRun(ctx);
    if (!m_lastPlanFile.empty()) {
        ImGui::SameLine();
        if (ui::GhostButton(ICON_EXTERNAL "  Open plan"))
            ctx.platform.OpenInExplorer(m_lastPlanFile);
        ImGui::SameLine();
        const bool current = m_dryRunFingerprint == m_plan.Fingerprint();
        ImGui::AlignTextToFramePadding();
        ui::ColoredText(current ? Success : Warning, current ? ICON_CHECK " plan is current" : ICON_WARNING " plan changed since the dry run");
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    // Why the button may be disabled, most important first.
    std::string why;
    const auto save = ctx.saveQueue.GetStats();
    if (!ctx.ActiveConnected())
        why = "Connect the phone.";
    else if (m_plan.Empty())
        why = "The plan is empty.";
    else if (m_plan.HasBlockers())
        why = "Fix the blocker above (KEEP files not saved).";
    else if (save.queued + save.active > 0)
        why = "Wait for the save queue to finish.";
    else if (ctx.settings.alwaysDryRun && m_dryRunFingerprint != m_plan.Fingerprint())
        why = "Do a dry run of this exact plan first.";
    else if (std::string_view(m_confirm) != "NUKE")
        why = "Type NUKE to confirm.";

    ImGui::AlignTextToFramePadding();
    ui::DimText("Type NUKE to confirm:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(140));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Mix(PanelRaised, Danger, 0.08f));
    ImGui::InputText("##confirm", m_confirm, sizeof(m_confirm), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, S(14));
    const std::string label = std::format(ICON_FIRE "  Nuke phone: delete {} files, uninstall {} apps", ui::Count(m_plan.del.files.size()), m_plan.uninstall.size());
    if (ui::DangerButton(label.c_str(), {}, why.empty()))
        Start(ctx);
    if (!why.empty()) {
        ImGui::SameLine(0, S(12));
        ImGui::AlignTextToFramePadding();
        ui::DimText(why);
    }
}

void NukePage::Start(AppContext& ctx)
{
    if (!ctx.adb)
        return;
    // Always log the exact plan that runs, dry run or not.
    DryRun(ctx);

    NukeTask t;
    t.serial = ctx.activeSerial;
    for (uint32_t i : m_plan.del.files)
        t.deletePaths.push_back(ctx.files.Files()[i].path);
    for (const auto& v : ctx.files.Volumes()) {
        t.volumeRoots.push_back(v.root);
        t.volumeLabels.push_back(v.label);
    }
    for (const auto& pkg : m_plan.uninstall)
        for (const auto& a : ctx.apps.Apps())
            if (a.package == pkg)
                t.apps.push_back({ a.package, a.apkPaths });
    t.backupApks = ctx.settings.backupApksBeforeUninstall;
    t.apkBackupDir = ctx.saveQueue.Folder() / L"apks";
    t.clearCaches = m_plan.clearCaches;
    t.tmpDir = Settings::DataDir() / L"tmp";

    m_progress = std::make_shared<NukeProgress>();
    m_finishedHandled = false;
    m_confirm[0] = 0;
    auto progress = m_progress;
    auto adb = ctx.adb;
    m_job = ctx.jobs.Submit("Nuking", JobPriority::High, [adb, t, progress](Job&, std::stop_token st) { RunNuke(*adb, t, *progress, st); });
}

void NukePage::Finish(AppContext& ctx)
{
    std::vector<std::string> deleted, uninstalled;
    {
        std::lock_guard l(m_progress->mutex);
        deleted = m_progress->deletedPaths;
        uninstalled = m_progress->uninstalled;
    }
    const size_t removed = ctx.files.RemovePaths(deleted);
    for (const auto& pkg : uninstalled)
        ctx.apps.SetFlaggedForUninstall(pkg, false);
    if (!uninstalled.empty())
        ctx.requestAppScan = true;
    m_ov = {};
    ++m_ovVersion;
    m_dryRunFingerprint = 0;
    LogInfo("Nuke finished: {} files removed from the list", removed);
}

void NukePage::DrawRunning(AppContext& ctx)
{
    NukePhase phase;
    size_t done, total, deleted;
    {
        std::lock_guard l(m_progress->mutex);
        phase = m_progress->phase;
        done = m_progress->phaseDone;
        total = m_progress->phaseTotal;
        deleted = m_progress->deleted;
    }
    ImGui::Dummy(ImVec2(0, S(16)));
    {
        Font f(GetFonts().uiStrong, FontH2);
        ui::ColoredText(Danger, ICON_FIRE "  Nuking\xE2\x80\xA6");
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    static constexpr NukePhase kPhases[] = { NukePhase::BackupApks, NukePhase::DeleteFiles, NukePhase::RemoveFolders, NukePhase::Uninstall,
        NukePhase::ClearCaches, NukePhase::MediaRescan };
    for (NukePhase ph : kPhases) {
        const bool current = ph == phase, past = static_cast<int>(ph) < static_cast<int>(phase);
        ImGui::AlignTextToFramePadding();
        if (current) {
            ui::Spinner(S(7), S(2), Danger);
            ImGui::SameLine(0, S(10));
            ImGui::TextUnformatted(PhaseName(ph));
            ImGui::SameLine(0, S(10));
            ui::DimText(std::format("{} / {}", ui::Count(done), ui::Count(total)));
            ui::ProgressBarThin(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImGui::GetContentRegionAvail().x, S(6), Danger);
        } else {
            ui::ColoredText(past ? Success : TextDim, past ? ICON_CHECK : ICON_CHEVRON_RIGHT);
            ImGui::SameLine(0, S(10));
            ui::ColoredText(past ? Text : TextDim, PhaseName(ph));
        }
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    ui::DimText(std::format("{} files deleted so far", ui::Count(deleted)));
    ImGui::Dummy(ImVec2(0, S(6)));
    if (ui::SecondaryButton(ICON_XMARK "  Cancel (stops after the current batch)") && m_job)
        m_job->Cancel();
    (void)ctx;
}

void NukePage::DrawResult(AppContext& ctx)
{
    // Keep the progress object alive for this whole function: "Done" resets
    // m_progress, and its mutex must not die while locked.
    const auto keep = m_progress;
    std::lock_guard l(keep->mutex);
    const auto& p = *keep;
    uint64_t freed = 0;
    for (size_t i = 0; i < std::min(p.before.size(), p.after.size()); ++i)
        if (p.after[i].free > p.before[i].free)
            freed += p.after[i].free - p.before[i].free;

    ImGui::Dummy(ImVec2(0, S(16)));
    {
        Font f(GetFonts().uiStrong, FontH1);
        if (p.cancelled)
            ui::ColoredText(Warning, std::format(ICON_WARNING "  Nuke cancelled. Freed {} so far", ui::Bytes(freed)));
        else
            ui::ColoredText(Success, std::format("Freed {} \xF0\x9F\x8E\x89", ui::Bytes(freed)));
    }
    ImGui::Dummy(ImVec2(0, S(10)));

    // Before / after per volume.
    for (size_t i = 0; i < std::min(p.before.size(), p.after.size()); ++i) {
        const auto& b = p.before[i];
        const auto& a = p.after[i];
        if (!b.total)
            continue;
        {
            Font f(GetFonts().uiStrong, FontBase);
            ImGui::TextUnformatted(b.label.c_str());
        }
        const float w = (ImGui::GetContentRegionAvail().x - S(20)) * 0.5f;
        ImGui::BeginGroup();
        ui::DimText(std::format("Before: {} free", ui::Bytes(b.free)));
        ui::StorageBar(static_cast<double>(b.used) / static_cast<double>(b.total), w, S(10));
        ImGui::EndGroup();
        ImGui::SameLine(0, S(20));
        ImGui::BeginGroup();
        ui::ColoredText(Success, std::format("After: {} free", ui::Bytes(a.free)));
        ui::StorageBar(a.total ? static_cast<double>(a.used) / static_cast<double>(a.total) : 0.0, w, S(10));
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, S(8)));
    }

    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextUnformatted(std::format(ICON_TRASH "  {} files deleted{}", ui::Count(p.deleted),
        p.deleteFailed ? std::format(", {} couldn't be deleted", ui::Count(p.deleteFailed)) : std::string())
                               .c_str());
    if (p.foldersRemoved)
        ImGui::TextUnformatted(std::format(ICON_FOLDER_OPEN "  {} empty folders removed", ui::Count(p.foldersRemoved)).c_str());
    if (p.apksBackedUp)
        ImGui::TextUnformatted(std::format(ICON_DOWNLOAD "  {} APKs backed up", p.apksBackedUp).c_str());
    if (!p.uninstalled.empty())
        ImGui::TextUnformatted(std::format(ICON_CUBES "  {} apps uninstalled", p.uninstalled.size()).c_str());
    for (const auto& [pkg, reason] : p.uninstallFailed)
        ui::ColoredText(Warning, std::format(ICON_WARNING "  {} not uninstalled: {}", pkg, reason));
    if (p.cachesCleared)
        ImGui::TextUnformatted(ICON_BROOM "  App caches cleared");
    if (!p.failedSamples.empty()) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ui::ColoredText(Warning, "Some files couldn't be deleted, for example:");
        Font f(GetFonts().mono, FontSmall);
        for (const auto& s : p.failedSamples)
            ui::DimText(s);
    }
    ImGui::Dummy(ImVec2(0, S(12)));
    if (ui::SecondaryButton(ICON_FILE_LINES "  Open log folder"))
        ctx.platform.OpenInExplorer(Settings::DataDir() / L"logs");
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_ROTATE "  Rescan the phone")) {
        ctx.requestScan = true;
        ctx.requestAppScan = true;
    }
    ImGui::SameLine();
    if (ui::PrimaryButton("Done"))
        m_progress.reset(); // back to the plan; `keep` holds the object until we return
}

} // namespace ps
