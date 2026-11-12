#include "screens/CleanupPage.h"

#include "adb/AdbClient.h"
#include "app/AppContext.h"
#include "scan/AppSession.h"
#include "scan/FileStore.h"
#include "scan/ScanSession.h"
#include "thumbs/ThumbCache.h"
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

struct MinSize {
    const char* label;
    uint64_t bytes;
};
constexpr MinSize kMinSizes[] = { { "Files over 4 KB", 4'096 }, { "Files over 100 KB", 100'000 }, { "Files over 1 MB", 1'000'000 },
    { "Files over 10 MB", 10'000'000 } };

} // namespace

const FileEntry* CleanupPage::Lookup(const AppContext& ctx, std::string_view path)
{
    if (ctx.files.Version() != m_byPathVersion) {
        m_byPath.clear();
        const auto& f = ctx.files.Files();
        m_byPath.reserve(f.size());
        for (uint32_t i = 0; i < f.size(); ++i)
            m_byPath.emplace(f[i].path, i);
        m_byPathVersion = ctx.files.Version();
    }
    auto it = m_byPath.find(path);
    return it == m_byPath.end() ? nullptr : &ctx.files.Files()[it->second];
}

void CleanupPage::Draw(AppContext& ctx)
{
    if (ctx.activeSerial != m_serial) {
        m_serial = ctx.activeSerial;
        if (m_dupJob)
            m_dupJob->Cancel();
        m_dup = std::make_shared<DupShared>();
        m_groups.clear();
        m_ticked.clear();
        m_searched = false;
        m_junkKey = ~0ull;
    }
    // Dev presets: --files=dups (start a search once the scan is done) / --files=junk.
    if (ctx.filesPreset == "junk") {
        ctx.filesPreset.clear();
        m_tab = 1;
    } else if (ctx.filesPreset == "compress") {
        ctx.filesPreset.clear();
        m_tab = 2;
    } else if (ctx.filesPreset == "dups" && !ctx.files.Files().empty() && !ctx.scan.Running()) {
        ctx.filesPreset.clear();
        m_tab = 0;
        StartFind(ctx);
    }
    ui::Tabs(m_tab, { ICON_FILES "  Duplicates", ICON_BROOM "  Junk", ICON_COMPRESS "  Compress" });

    // Right side: real free space per volume (refreshed by every scan) + Rescan,
    // so the effect of a cleanup can be checked against the phone itself.
    {
        std::string freeText;
        for (const auto& v : ctx.files.Volumes())
            if (v.totalBytes)
                freeText += std::format("{}{}: {} free", freeText.empty() ? "" : "  \xC2\xB7  ", v.label, ui::Bytes(v.freeBytes));
        const bool scanning = ctx.scan.Running();
        const char* label = scanning ? "Rescanning\xE2\x80\xA6" : ICON_ROTATE "  Rescan";
        const float bw = ui::TextWidth(label) + S(28);
        const float textW = ui::TextWidth(freeText);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - bw - textW - S(16)));
        ImGui::AlignTextToFramePadding();
        ui::DimText(freeText);
        ImGui::SameLine(0, S(16));
        if (ui::SecondaryButton(label, ImVec2(bw, 0), !scanning && !ctx.deleteRunning && ctx.ActiveConnected())) {
            ctx.requestScan = true;
            ctx.requestAppScan = true;
            m_junkKey = ~0ull;
        }
        ImGui::SetItemTooltip("Re-reads every file and the free space from the phone.");
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    if (ctx.files.Files().empty()) {
        ui::EmptyState(ICON_BROOM, ctx.scan.Running() ? "Scanning\xE2\x80\xA6" : "Scan the phone first",
            "Cleanup works from the file scan (Overview \xE2\x86\x92 Rescan).");
        return;
    }
    if (m_tab == 0)
        DrawDuplicates(ctx);
    else if (m_tab == 1)
        DrawJunk(ctx);
    else
        m_compress.Draw(ctx);
    DrawConfirm(ctx);
}

// ---- Duplicates ----------------------------------------------------------------

void CleanupPage::StartFind(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    if (m_dupJob)
        m_dupJob->Cancel();
    m_dup = std::make_shared<DupShared>();
    m_dup->running = true;
    m_searched = true;
    m_groups.clear();
    m_ticked.clear();
    auto shared = m_dup;
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial;
    FileStore snapshot = ctx.files; // the job must never touch the UI's live store
    const uint64_t minSize = kMinSizes[m_minSize].bytes;
    m_dupJob = ctx.jobs.Submit("Finding duplicates", JobPriority::Normal,
        [shared, adb, serial, snapshot = std::move(snapshot), minSize](Job& job, std::stop_token st) {
            auto groups = FindDuplicates(*adb, serial, snapshot, minSize, st, [&](const DupProgress& p) {
                job.SetDetail(std::format("{} {}/{}", p.phase, p.done, p.total));
                std::lock_guard l(shared->mutex);
                shared->progress = p;
            });
            std::lock_guard l(shared->mutex);
            shared->groups = std::move(groups);
            shared->running = false;
            shared->done = true;
        });
}

void CleanupPage::DrawDuplicates(AppContext& ctx)
{
    bool running;
    DupProgress progress;
    {
        std::lock_guard l(m_dup->mutex);
        running = m_dup->running;
        progress = m_dup->progress;
        if (m_dup->done) {
            m_dup->done = false;
            m_groups = std::move(m_dup->groups);
            m_ticked.assign(m_groups.size(), true); // every group pre-selected; the keeper is suggested per group
        }
    }

    // Drop groups/copies that no longer exist (deleted by a cleanup or nuke).
    if (!running && !m_groups.empty()) {
        for (size_t g = 0; g < m_groups.size(); ++g) {
            auto& grp = m_groups[g];
            const std::string keepPath = grp.keep < grp.files.size() ? grp.files[grp.keep].path : std::string();
            std::erase_if(grp.files, [&](const DupFile& f) { return !Lookup(ctx, f.path); });
            auto it = std::ranges::find(grp.files, keepPath, &DupFile::path);
            grp.keep = it != grp.files.end() ? static_cast<size_t>(it - grp.files.begin()) : 0;
        }
        for (size_t g = m_groups.size(); g-- > 0;)
            if (m_groups[g].files.size() < 2) {
                m_groups.erase(m_groups.begin() + static_cast<long>(g));
                m_ticked.erase(m_ticked.begin() + static_cast<long>(g));
            }
    }

    // Toolbar.
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::BeginCombo("##minsize", kMinSizes[m_minSize].label)) {
        for (int i = 0; i < static_cast<int>(std::size(kMinSizes)); ++i)
            if (ImGui::Selectable(kMinSizes[i].label, m_minSize == i))
                m_minSize = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (running) {
        if (ui::SecondaryButton(ICON_XMARK "  Stop") && m_dupJob)
            m_dupJob->Cancel();
    } else if (ui::PrimaryButton(m_searched ? ICON_ROTATE "  Search again" : ICON_SEARCH "  Find duplicates", {}, ctx.ActiveConnected())) {
        StartFind(ctx);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ui::DimText("Same size \xE2\x86\x92 same first 64 KB \xE2\x86\x92 same full MD5 (checked on the phone). Nothing is deleted on a guess.");

    if (running) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ui::DimText(std::format("{}: {} / {}", progress.phase.empty() ? std::string("Starting") : progress.phase, ui::Count(progress.done),
            ui::Count(progress.total)));
        ui::ProgressBarThin(progress.total ? static_cast<float>(progress.done) / static_cast<float>(progress.total) : 0.0f,
            ImGui::GetContentRegionAvail().x, S(6), Accent);
        return;
    }
    if (!m_searched) {
        ui::EmptyState(ICON_FILES, "Find identical files", "WhatsApp saves the same photo several times (sent, received, forwarded). Click Find duplicates.");
        return;
    }
    if (m_groups.empty()) {
        ui::EmptyState(ICON_CIRCLE_CHECK, "No duplicates found \xF0\x9F\x8E\x89", "Every file in this size range is unique.");
        return;
    }

    // Summary + actions.
    uint64_t reclaim = 0, total = 0;
    size_t copies = 0;
    for (size_t g = 0; g < m_groups.size(); ++g) {
        total += m_groups[g].Reclaimable();
        if (m_ticked[g]) {
            reclaim += m_groups[g].Reclaimable();
            copies += m_groups[g].files.size() - 1;
        }
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::TextUnformatted(std::format("{} groups of identical files \xC2\xB7 {} could be freed", ui::Count(m_groups.size()), ui::Bytes(total)).c_str());
    }
    if (ui::GhostButton("Select all"))
        m_ticked.assign(m_groups.size(), true);
    ImGui::SameLine();
    if (ui::GhostButton("Select none"))
        m_ticked.assign(m_groups.size(), false);
    ImGui::SameLine();
    const std::string del = std::format(ICON_TRASH "  Delete {} extra copies ({})", ui::Count(copies), ui::Bytes(reclaim));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ui::TextWidth(del) - S(30)));
    if (ui::DangerButton(del.c_str(), {}, copies > 0 && ctx.ActiveConnected() && !ctx.deleteRunning)) {
        m_pending = {};
        m_pending.paths = DuplicateDeleteList(m_groups, m_ticked, ctx.files);
        for (size_t g = 0; g < m_groups.size(); ++g)
            if (m_ticked[g])
                m_pending.mustExist.push_back(m_groups[g].files[m_groups[g].keep].path);
        m_pending.label = "duplicate copies";
        m_pendingBytes = 0;
        for (const auto& p : m_pending.paths)
            if (const auto* e = Lookup(ctx, p))
                m_pendingBytes += e->size;
        m_confirmOpen = true;
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    // Flattened rows: group header + one row per copy.
    struct Row {
        int group;
        int file; // -1 = header
    };
    std::vector<Row> rows;
    for (int g = 0; g < static_cast<int>(m_groups.size()); ++g) {
        rows.push_back({ g, -1 });
        for (int f = 0; f < static_cast<int>(m_groups[g].files.size()); ++f)
            rows.push_back({ g, f });
    }
    const float rowH = S(44);
    ImGui::BeginChild("##dups", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const Row row = rows[r];
            auto& grp = m_groups[row.group];
            ImGui::PushID(row.group * 4096 + row.file + 1);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            if (row.file < 0) {
                // Group header.
                dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), U32(PanelRaised));
                ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
                bool t = m_ticked[row.group];
                if (ImGui::Checkbox("##g", &t))
                    m_ticked[row.group] = t;
                ImGui::SameLine(0, S(10));
                ImGui::AlignTextToFramePadding();
                {
                    Font f(GetFonts().uiStrong, FontBase);
                    ImGui::TextUnformatted(std::format("{} identical copies \xC2\xB7 {} each", grp.files.size(), ui::Bytes(grp.size)).c_str());
                }
                ImGui::SameLine(0, S(10));
                ui::ColoredText(m_ticked[row.group] ? Danger : TextDim, std::format("frees {}", ui::Bytes(grp.Reclaimable())));
            } else {
                const DupFile& df = grp.files[row.file];
                const FileEntry* e = Lookup(ctx, df.path);
                const bool keeper = static_cast<size_t>(row.file) == grp.keep;
                const bool keepFlag = e && (e->flags & FileFlag_Keep);
                // Radio: which copy stays.
                ImGui::SetCursorScreenPos(ImVec2(p.x + S(34), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
                if (ui::Radio("##keep", keeper))
                    grp.keep = static_cast<size_t>(row.file);
                ImGui::SetItemTooltip("Keep this copy");
                // Thumbnail.
                const float th = rowH - S(8);
                const ImVec2 tp(p.x + S(70), p.y + S(4));
                dl->AddRectFilled(tp, ImVec2(tp.x + th, tp.y + th), U32(Border, 0.6f), S(4));
                if (e) {
                    const ThumbView v = ctx.thumbs.Request(*e, ThumbVariant::Grid);
                    if (v.state == ThumbState::Ready && v.texture) {
                        // centre-crop
                        const float a = static_cast<float>(v.w) / static_cast<float>(std::max(1, v.h));
                        ImVec2 uv0(0, 0), uv1(1, 1);
                        if (a > 1) {
                            uv0.x = (1 - 1 / a) * 0.5f;
                            uv1.x = 1 - uv0.x;
                        } else {
                            uv0.y = (1 - a) * 0.5f;
                            uv1.y = 1 - uv0.y;
                        }
                        dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(v.texture)), tp, ImVec2(tp.x + th, tp.y + th), uv0, uv1, U32Raw(ImVec4(1, 1, 1, 1)), S(4));
                    } else {
                        Font f(GetFonts().ui, FontSmall);
                        const char* icon = ui::CategoryIcon(static_cast<uint8_t>(e->category));
                        const ImVec2 ts = ImGui::CalcTextSize(icon);
                        dl->AddText(ImVec2(tp.x + (th - ts.x) * 0.5f, tp.y + (th - ts.y) * 0.5f), U32(ui::CategoryColor(static_cast<uint8_t>(e->category))), icon);
                    }
                }
                // Text.
                const float tx = tp.x + th + S(12);
                const float lh = ImGui::GetTextLineHeight();
                const float y0 = p.y + (rowH - lh * 2) * 0.5f;
                const std::string name = df.path.substr(df.path.rfind('/') + 1);
                std::string folder = df.path.substr(0, df.path.rfind('/'));
                if (folder.starts_with("/storage/emulated/0/"))
                    folder = folder.substr(20);
                else if (folder.starts_with("/storage/"))
                    folder = "SD card \xC2\xB7 " + folder.substr(folder.find('/', 9) + 1);
                dl->AddText(ImVec2(tx, y0), U32(keeper ? Text : (m_ticked[row.group] && !keepFlag ? Mix(Text, Danger, 0.35f) : TextDim)), name.c_str());
                {
                    Font f(GetFonts().ui, FontSmall);
                    dl->AddText(ImVec2(tx, y0 + lh), U32(TextDim), std::format("{}  \xC2\xB7  {}", folder, ui::Date(df.mtime)).c_str());
                }
                // Right-side state.
                const char* tag = keeper ? "KEEPS" : keepFlag ? "KEEP (never deleted)" : m_ticked[row.group] ? "DELETE" : "";
                const ImVec4 tagCol = keeper ? Accent : keepFlag ? Accent : Danger;
                if (*tag) {
                    Font f(GetFonts().uiStrong, FontSmall - 1);
                    const ImVec2 ts = ImGui::CalcTextSize(tag);
                    const ImVec2 b1(p.x + w - S(12), p.y + (rowH + ts.y) * 0.5f + S(2));
                    const ImVec2 b0(b1.x - ts.x - S(14), b1.y - ts.y - S(4));
                    dl->AddRectFilled(b0, b1, U32(tagCol, 0.16f), (b1.y - b0.y) * 0.5f);
                    dl->AddText(ImVec2(b0.x + S(7), b0.y + S(2)), U32(tagCol), tag);
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            ImGui::Dummy(ImVec2(w, 0));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// ---- Junk ------------------------------------------------------------------------

void CleanupPage::DrawJunk(AppContext& ctx)
{
    const auto mode = static_cast<ProtectorMode>(std::clamp(ctx.settings.nukeMode, 0, 1));
    const uint64_t key = ctx.files.Version() * 1000003ull ^ ctx.verified.Version() * 7919ull ^ ctx.apps.Version() * 31ull ^ static_cast<uint64_t>(mode);
    if (key != m_junkKey) {
        std::set<std::string, std::less<>> installed;
        for (const auto& a : ctx.apps.Apps())
            installed.insert(a.package);
        m_junk = BuildJunk(ctx.files, ctx.verified, installed, mode);
        if (m_junkTicked.size() != m_junk.size()) {
            m_junkTicked.resize(m_junk.size());
            for (size_t k = 0; k < m_junk.size(); ++k)
                m_junkTicked[k] = GetJunkInfo(m_junk[k].kind).safe; // review categories start unticked
            m_junkOpen.assign(m_junk.size(), false);
        }
        m_junkKey = key;
    }

    ImGui::AlignTextToFramePadding();
    ui::DimText(mode == ProtectorMode::Prompt ? "Nuke prompt: SAFE junk can always go; REVIEW junk only if it's saved on the PC."
                                              : "Nuke free: everything you tick is deleted.");
    if (ctx.apps.Apps().empty()) {
        ImGui::SameLine();
        ui::ColoredText(Warning, ICON_WARNING "  App scan missing: leftovers of uninstalled apps can't be detected yet.");
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    uint64_t selBytes = 0;
    size_t selFiles = 0;
    const float footer = S(56);
    ImGui::BeginChild("##junk", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_Borders);
    for (size_t k = 0; k < m_junk.size(); ++k) {
        const auto& c = m_junk[k];
        const auto& info = GetJunkInfo(c.kind);
        if (c.files.empty() && c.blocked.empty())
            continue;
        ImGui::PushID(static_cast<int>(k));
        bool t = m_junkTicked[k];
        if (ImGui::Checkbox("##t", &t))
            m_junkTicked[k] = t;
        ImGui::SameLine(0, S(10));
        ImGui::BeginGroup();
        {
            Font f(GetFonts().uiStrong, FontBase);
            ImGui::TextUnformatted(info.name);
        }
        ImGui::SameLine(0, S(8));
        ui::Pill(info.safe ? "SAFE" : "REVIEW", info.safe ? Success : Warning);
        ImGui::SameLine(0, S(12));
        ImGui::TextUnformatted(std::format("{} files \xC2\xB7 {}", ui::Count(c.files.size()), ui::Bytes(c.bytes)).c_str());
        {
            Font f(GetFonts().ui, FontSmall);
            ui::DimText(info.why);
            if (!c.blocked.empty())
                ui::ColoredText(Accent, std::format(ICON_SHIELD "  {} more ({}) aren't saved, so Nuke prompt keeps them.", ui::Count(c.blocked.size()),
                                            ui::Bytes(c.blockedBytes)));
        }
        if (!c.files.empty() && ui::GhostButton(m_junkOpen[k] ? ICON_CHEVRON_DOWN "  Hide files" : ICON_CHEVRON_RIGHT "  Show files"))
            m_junkOpen[k] = !m_junkOpen[k];
        if (m_junkOpen[k]) {
            std::vector<uint32_t> top = c.files;
            const auto& f = ctx.files.Files();
            const size_t n = std::min<size_t>(40, top.size());
            std::partial_sort(top.begin(), top.begin() + n, top.end(), [&](uint32_t a, uint32_t b) { return f[a].size > f[b].size; });
            Font mf(GetFonts().mono, FontSmall);
            for (size_t i = 0; i < n; ++i)
                ui::DimText(std::format("{:>10}  {}", ui::Bytes(f[top[i]].size), std::string(f[top[i]].RelPath())));
            if (top.size() > n)
                ui::DimText(std::format("\xE2\x80\xA6 and {} more", ui::Count(top.size() - n)));
        }
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::PopID();
        if (m_junkTicked[k]) {
            selBytes += c.bytes;
            selFiles += c.files.size();
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, S(6)));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(std::format("Selected: {} files \xC2\xB7 {}", ui::Count(selFiles), ui::Bytes(selBytes)).c_str());
    }
    ImGui::SameLine();
    const std::string del = std::format(ICON_BROOM "  Clean selected ({})", ui::Bytes(selBytes));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ui::TextWidth(del) - S(30)));
    if (ui::DangerButton(del.c_str(), {}, selFiles > 0 && ctx.ActiveConnected() && !ctx.deleteRunning)) {
        m_pending = {};
        for (size_t k = 0; k < m_junk.size(); ++k)
            if (m_junkTicked[k])
                for (uint32_t i : m_junk[k].files)
                    m_pending.paths.push_back(ctx.files.Files()[i].path);
        m_pending.label = "junk files";
        m_pendingBytes = selBytes;
        m_confirmOpen = true;
    }
}

void CleanupPage::DrawConfirm(AppContext& ctx)
{
    if (m_confirmOpen) {
        ImGui::OpenPopup("Delete from the phone?");
        m_confirmOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(480), 0));
    if (ImGui::BeginPopupModal("Delete from the phone?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("Delete %s %s (%s) from the phone? This can't be undone.", ui::Count(m_pending.paths.size()).c_str(), m_pending.label.c_str(),
            ui::Bytes(m_pendingBytes).c_str());
        if (!m_pending.mustExist.empty()) {
            Font f(GetFonts().ui, FontSmall);
            ui::DimText(std::format("One copy of each of the {} groups stays; PhoneSweep checks it's still there first.", m_pending.mustExist.size()));
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ui::DangerButton(ICON_TRASH "  Delete")) {
            ctx.requestDelete = std::move(m_pending);
            m_pending = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace ps
