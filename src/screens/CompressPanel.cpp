#include "screens/CompressPanel.h"

#include "adb/AdbClient.h"
#include "app/AppContext.h"
#include "nuke/NukePlan.h"
#include "scan/FileStore.h"
#include "scan/ScanSession.h"
#include "transfer/Manifest.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace ps {

using namespace ps::theme;
using namespace std::chrono_literals;

namespace {

struct OutcomeInfo {
    const char* title;
    const char* detail;
};
constexpr OutcomeInfo kOutcomes[] = {
    { "Compressed on the phone AND on the PC",
        "The phone keeps the smaller file in place of the original; a copy of it is also saved in the backup folder." },
    { "Compressed on the phone only", "The phone keeps the smaller file in place of the original. Nothing extra is saved on the PC." },
    { "Compressed on the PC only, removed from the phone",
        "The smaller file is saved in the backup folder and the original is deleted from the phone. Frees the most space." },
};

std::string Folder(std::string_view path)
{
    std::string folder(path.substr(0, path.rfind('/')));
    if (folder.starts_with("/storage/emulated/0/"))
        return folder.substr(20);
    if (folder.starts_with("/storage/")) {
        const auto s = folder.find('/', 9);
        return "SD card \xC2\xB7 " + (s == std::string::npos ? std::string() : folder.substr(s + 1));
    }
    return folder;
}

} // namespace

// ---- Background work -------------------------------------------------------------

void CompressPanel::StartQuery(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    if (m_queryJob)
        m_queryJob->Cancel();
    m_shared = std::make_shared<MediaShared>();
    m_shared->running = true;
    m_queried = true;
    auto shared = m_shared;
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial;
    const auto ffmpeg = ctx.ffmpeg;
    const auto tmp = Settings::DataDir() / L"compress_tmp";
    const bool probe = !m_qsv.has_value();
    m_queryJob = ctx.jobs.Submit("Reading photo/video sizes", JobPriority::Normal, [shared, adb, serial, ffmpeg, tmp, probe](Job&, std::stop_token st) {
        // MediaStore already knows duration and resolution of every photo and video.
        std::map<std::string, MediaInfo, std::less<>> media;
        std::string error;
        const struct {
            const char* uri;
            const char* projection;
        } queries[] = {
            { "content://media/external/video/media", "_data:duration:width:height:_size" },
            { "content://media/external/images/media", "_data:width:height:_size" },
        };
        for (const auto& q : queries) {
            const auto r = adb->Shell(serial, ShellCommand("content").arg("query").arg("--uri").arg(q.uri).arg("--projection").arg(q.projection),
                { .timeout = 120s, .cancel = st });
            if (!r.ok() && r.out.empty()) {
                error = "Couldn't read the gallery index from the phone.";
                continue;
            }
            media.merge(ParseMediaInfoRows(r.out));
        }
        std::optional<bool> qsv;
        if (probe && ffmpeg && !st.stop_requested())
            qsv = ProbeQsv(*ffmpeg, tmp);
        LogInfo("Compress: {} media rows from MediaStore{}", media.size(), qsv ? (*qsv ? ", Quick Sync available" : ", no Quick Sync") : "");
        std::lock_guard l(shared->mutex);
        shared->media = std::move(media);
        shared->error = std::move(error);
        if (qsv)
            shared->qsv = qsv;
        shared->running = false;
        shared->done = true;
    });
}

void CompressPanel::StartCompress(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ffmpeg || !ctx.ActiveConnected())
        return;
    CompressTask task;
    task.serial = ctx.activeSerial;
    for (const auto& c : m_cands)
        if (m_ticked.contains(c.path))
            task.items.push_back(c);
    if (task.items.empty())
        return;
    task.outcome = static_cast<CompressOutcome>(m_outcome);
    task.ffmpeg = *ctx.ffmpeg;
    task.useQsv = m_useGpu && m_qsv.value_or(false);
    task.pcFolder = ctx.saveFolder / L"Compressed";
    for (const auto& v : ctx.files.Volumes()) {
        task.volumeRoots.push_back(v.root);
        task.volumeLabels.push_back(v.label);
    }
    task.tmpDir = Settings::DataDir() / L"compress_tmp";
    LogInfo("Compress: {} files, outcome {}, encoder {}", task.items.size(), m_outcome, task.useQsv ? "h264_qsv" : "libx264");

    m_progress = std::make_shared<CompressProgress>();
    auto progress = m_progress;
    auto adb = ctx.adb;
    m_runJob = ctx.jobs.Submit("Compressing", JobPriority::Normal, [adb, task = std::move(task), progress](Job& job, std::stop_token st) {
        RunCompress(*adb, task, *progress, st);
        job.SetDetail("done");
    });
    for (const auto& c : task.items)
        m_ticked.erase(c.path);
}

// ---- Drawing ------------------------------------------------------------------------

void CompressPanel::Draw(AppContext& ctx)
{
    if (ctx.activeSerial != m_serial) {
        m_serial = ctx.activeSerial;
        if (m_queryJob)
            m_queryJob->Cancel();
        m_shared = std::make_shared<MediaShared>();
        m_queried = false;
        m_media.clear();
        m_cands.clear();
        m_ticked.clear();
        m_candKey = ~0ull;
    }
    {
        auto shared = m_shared; // keep the mutex alive while it's locked
        std::lock_guard l(shared->mutex);
        if (shared->done) {
            shared->done = false;
            m_media = std::move(shared->media);
            if (shared->qsv)
                m_qsv = shared->qsv;
            if (!shared->error.empty())
                ui::Toast(shared->error, ui::ToastKind::Warning);
            m_candKey = ~0ull;
        }
    }

    if (m_progress) {
        bool done;
        {
            auto keep = m_progress;
            std::lock_guard l(keep->mutex);
            done = keep->done;
        }
        if (done)
            DrawResults(ctx);
        else
            DrawRunning(ctx);
        return;
    }
    DrawList(ctx);
    DrawChoice(ctx);
}

void CompressPanel::DrawList(AppContext& ctx)
{
    bool running;
    {
        auto shared = m_shared;
        std::lock_guard l(shared->mutex);
        running = shared->running;
    }
    if (!m_queried && !running && ctx.ActiveConnected() && !ctx.scan.Running())
        StartQuery(ctx); // cheap: two MediaStore queries

    if (!ctx.ffmpeg) {
        ui::EmptyState(ICON_FILM, "ffmpeg is needed to compress videos",
            "Install it (winget install ffmpeg) or point Settings \xE2\x86\x92 Thumbnails at ffmpeg.exe, then come back.");
        return;
    }
    if (running || !m_queried) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ui::DimText("Reading photo and video sizes from the phone\xE2\x80\xA6");
        ui::SkeletonRows(6, S(44));
        return;
    }

    const auto mode = static_cast<ProtectorMode>(std::clamp(ctx.settings.nukeMode, 0, 1));
    const uint64_t key = ctx.files.Version() * 1000003ull ^ ctx.verified.Version() * 7919ull ^ m_media.size();
    if (key != m_candKey) {
        m_cands = FindCompressCandidates(ctx.files, ctx.verified, m_media);
        std::set<std::string, std::less<>> still;
        for (const auto& c : m_cands)
            if (m_ticked.contains(c.path))
                still.insert(c.path);
        m_ticked = std::move(still);
        m_candKey = key;
    }

    // Toolbar.
    const char* filters[] = { "Photos and videos", "Videos only", "Photos only" };
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::BeginCombo("##cfilter", filters[m_filter])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(filters[i], m_filter == i))
                m_filter = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_ROTATE "  Refresh", {}, ctx.ActiveConnected()))
        StartQuery(ctx);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ui::DimText("Balanced: H.264, at most 1080p, photos at JPEG 85. Dates, GPS and rotation are kept.");
    if (m_qsv.value_or(false)) {
        ImGui::SameLine(0, S(16));
        ImGui::Checkbox("Fast (Intel GPU)", &m_useGpu);
        ImGui::SetItemTooltip("Quick Sync encodes ~4x faster; CPU (x264) gives slightly smaller files.");
    }

    auto visible = [&](const CompressCandidate& c) {
        return m_filter == 0 || (m_filter == 1 && c.kind == MediaKind::Video) || (m_filter == 2 && c.kind == MediaKind::Photo);
    };
    if (m_cands.empty()) {
        ui::EmptyState(ICON_CIRCLE_CHECK, "Nothing worth compressing",
            "Only photos and videos that should shrink by at least 20% are listed. Phones that already record in HEVC usually have little to gain.");
        return;
    }

    uint64_t total = 0, selSaving = 0, selSize = 0;
    size_t sel = 0, selUnsaved = 0;
    std::vector<uint32_t> unsavedIdx;
    for (const auto& c : m_cands) {
        total += c.EstimatedSaving();
        if (m_ticked.contains(c.path)) {
            ++sel;
            selSaving += c.EstimatedSaving();
            selSize += c.size;
            if (!c.saved) {
                ++selUnsaved;
                unsavedIdx.push_back(c.file);
            }
        }
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::TextUnformatted(std::format("{} files could shrink \xC2\xB7 about {} to gain", ui::Count(m_cands.size()), ui::Bytes(total)).c_str());
    }
    if (ui::GhostButton("Select all")) {
        for (const auto& c : m_cands)
            if (visible(c))
                m_ticked.insert(c.path);
    }
    ImGui::SameLine();
    if (ui::GhostButton("Select none"))
        m_ticked.clear();
    ImGui::Dummy(ImVec2(0, S(2)));

    // Rows.
    std::vector<const CompressCandidate*> rows;
    for (const auto& c : m_cands)
        if (visible(c))
            rows.push_back(&c);
    const float footer = S(64);
    const float rowH = S(44);
    ImGui::BeginChild("##cands", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto& c = *rows[r];
            ImGui::PushID(r);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
            bool t = m_ticked.contains(c.path);
            if (ImGui::Checkbox("##t", &t)) {
                if (t)
                    m_ticked.insert(c.path);
                else
                    m_ticked.erase(c.path);
            }
            const float lh = ImGui::GetTextLineHeight();
            const float y0 = p.y + (rowH - lh * 2) * 0.5f;
            const float ix = p.x + S(44);
            dl->AddText(ImVec2(ix, y0 + lh * 0.5f), U32(c.kind == MediaKind::Video ? ui::CategoryColor(static_cast<uint8_t>(Category::Videos)) : ui::CategoryColor(static_cast<uint8_t>(Category::Images))),
                c.kind == MediaKind::Video ? ICON_FILM : ICON_IMAGE);
            const float tx = ix + S(30);
            const std::string name = c.path.substr(c.path.rfind('/') + 1);
            dl->AddText(ImVec2(tx, y0), U32(Text), name.c_str());
            {
                Font f(GetFonts().ui, FontSmall);
                std::string meta = std::format("{}  \xC2\xB7  {}\xC3\x97{}", Folder(c.path), c.info.width, c.info.height);
                if (c.kind == MediaKind::Video)
                    meta += std::format("  \xC2\xB7  {}:{:02}", c.info.durationMs / 60000, (c.info.durationMs / 1000) % 60);
                dl->AddText(ImVec2(tx, y0 + lh), U32(TextDim), meta.c_str());
            }
            // Right: size -> estimate, saved state.
            {
                const std::string sizes = std::format("{} \xE2\x86\x92 ~{}", ui::Bytes(c.size), ui::Bytes(c.estimatedSize));
                const char* tag = c.saved ? "SAVED ON PC" : "NOT SAVED";
                const ImVec4 tagCol = c.saved ? Success : (mode == ProtectorMode::Prompt ? Warning : TextDim);
                Font f(GetFonts().uiStrong, FontSmall - 1);
                const ImVec2 ts = ImGui::CalcTextSize(tag);
                const ImVec2 b1(p.x + w - S(12), p.y + (rowH + ts.y) * 0.5f + S(2));
                const ImVec2 b0(b1.x - ts.x - S(14), b1.y - ts.y - S(4));
                dl->AddRectFilled(b0, b1, U32(tagCol, 0.16f), (b1.y - b0.y) * 0.5f);
                dl->AddText(ImVec2(b0.x + S(7), b0.y + S(2)), U32(tagCol), tag);
                const float sw = ImGui::CalcTextSize(sizes.c_str()).x;
                dl->AddText(ImVec2(b0.x - sw - S(16), p.y + (rowH - ts.y) * 0.5f), U32(Success), sizes.c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            ImGui::Dummy(ImVec2(w, 0));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Footer: Nuke prompt wants the originals safely on the PC before anything is replaced.
    ImGui::Dummy(ImVec2(0, S(6)));
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(std::format("Selected: {} files \xC2\xB7 {} \xE2\x86\x92 saves ~{}", ui::Count(sel), ui::Bytes(selSize), ui::Bytes(selSaving)).c_str());
    }
    const bool blocked = mode == ProtectorMode::Prompt && selUnsaved > 0;
    if (blocked) {
        Font f(GetFonts().ui, FontSmall);
        ui::ColoredText(Accent, std::format(ICON_SHIELD "  Nuke prompt: {} original{} must be saved on the PC first (Nuke free in Settings skips this).",
                                    ui::Count(selUnsaved), selUnsaved == 1 ? "" : "s"));
    }
    ImGui::SameLine();
    const std::string go = std::format(ICON_COMPRESS "  Compress {} files", ui::Count(sel));
    const std::string save = std::format(ICON_DOWNLOAD "  Save {} originals first", ui::Count(selUnsaved));
    const std::string& label = blocked ? save : go;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ui::TextWidth(label) - S(30)));
    if (blocked) {
        if (ui::PrimaryButton(label.c_str(), {}, ctx.ActiveConnected() && !ctx.deleteRunning)) {
            ctx.requestSaveFiles.insert(ctx.requestSaveFiles.end(), unsavedIdx.begin(), unsavedIdx.end());
            ui::Toast(std::format("Saving {} originals \xE2\x80\x94 Compress unlocks once they're verified", unsavedIdx.size()), ui::ToastKind::Info);
        }
    } else if (ui::PrimaryButton(label.c_str(), {}, sel > 0 && ctx.ActiveConnected() && !ctx.deleteRunning)) {
        m_choiceOpen = true;
    }
}

void CompressPanel::DrawChoice(AppContext& ctx)
{
    if (m_choiceOpen) {
        ImGui::OpenPopup("Where should the compressed files go?");
        m_choiceOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(540), 0));
    if (ImGui::BeginPopupModal("Where should the compressed files go?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        for (int i = 0; i < 3; ++i) {
            ImGui::PushID(i);
            if (ui::Radio(kOutcomes[i].title, m_outcome == i))
                m_outcome = i;
            ImGui::Indent(S(28));
            {
                Font f(GetFonts().ui, FontSmall);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(470));
                ui::DimText(kOutcomes[i].detail);
                ImGui::PopTextWrapPos();
            }
            ImGui::Unindent(S(28));
            ImGui::Dummy(ImVec2(0, S(4)));
            ImGui::PopID();
        }
        if (m_outcome != 1) {
            Font f(GetFonts().mono, FontSmall);
            ImGui::PushTextWrapPos(0);
            ui::DimText(Narrow((ctx.saveFolder / L"Compressed").wstring()));
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        {
            Font f(GetFonts().ui, FontSmall);
            ImGui::PushTextWrapPos(0);
            ui::DimText("Each file is checked before anything on the phone changes: the compressed file must be smaller, upload with a matching "
                        "MD5, and keep its date. If anything fails, the original stays.");
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        if (ui::PrimaryButton(ICON_COMPRESS "  Start")) {
            StartCompress(ctx);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void CompressPanel::DrawRunning(AppContext&)
{
    size_t index, total, results;
    std::string current, step;
    float frac;
    {
        auto keep = m_progress;
        std::lock_guard l(keep->mutex);
        index = keep->index;
        total = keep->total;
        current = keep->current;
        step = keep->step;
        frac = keep->fileFraction;
        results = keep->results.size();
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    {
        Font f(GetFonts().uiStrong, FontBase + 2);
        ImGui::TextUnformatted(std::format("Compressing {} of {}", std::min(index + 1, std::max<size_t>(total, 1)), total).c_str());
    }
    ui::DimText(current.empty() ? std::string("Starting\xE2\x80\xA6") : std::format("{} \xE2\x80\x94 {}", current, step));
    ImGui::Dummy(ImVec2(0, S(6)));
    const float overall = total ? (static_cast<float>(results) + std::clamp(frac, 0.0f, 1.0f)) / static_cast<float>(total) : 0.0f;
    ui::ProgressBarThin(std::clamp(frac, 0.0f, 1.0f), ImGui::GetContentRegionAvail().x, S(6), Info);
    ImGui::Dummy(ImVec2(0, S(4)));
    ui::ProgressBarThin(overall, ImGui::GetContentRegionAvail().x, S(6), Accent);
    ImGui::Dummy(ImVec2(0, S(10)));
    if (ui::SecondaryButton(ICON_XMARK "  Stop after this file") && m_runJob)
        m_runJob->Cancel();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ui::DimText("Stopping never leaves a half-written file on the phone.");
}

void CompressPanel::DrawResults(AppContext& ctx)
{
    std::vector<CompressItemResult> results;
    {
        auto keep = m_progress;
        std::lock_guard l(keep->mutex);
        results = keep->results;
    }
    uint64_t before = 0, after = 0;
    size_t ok = 0;
    for (const auto& r : results)
        if (r.ok) {
            ++ok;
            before += r.before;
            after += r.after;
        }
    ImGui::Dummy(ImVec2(0, S(10)));
    {
        Font f(GetFonts().uiStrong, FontBase + 2);
        ImGui::TextUnformatted(std::format(ICON_CIRCLE_CHECK "  {} of {} files compressed \xC2\xB7 {} \xE2\x86\x92 {} (saved {})", ok, results.size(),
            ui::Bytes(before), ui::Bytes(after), ui::Bytes(before > after ? before - after : 0))
                                   .c_str());
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::BeginChild("##cres", ImVec2(0, ImGui::GetContentRegionAvail().y - S(52)), ImGuiChildFlags_Borders);
    for (const auto& r : results) {
        const std::string name = r.path.substr(r.path.rfind('/') + 1);
        ui::ColoredText(r.ok ? Success : Warning, r.ok ? ICON_CIRCLE_CHECK : ICON_WARNING);
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ui::DimText(r.ok ? std::format("{} \xE2\x86\x92 {} \xC2\xB7 {}", ui::Bytes(r.before), ui::Bytes(r.after), r.status) : r.status);
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(6)));
    if (ui::PrimaryButton(ICON_CHECK "  Done \xE2\x80\x94 rescan the phone")) {
        m_progress.reset();
        m_queried = false; // MediaStore changed too
        ctx.requestScan = true;
    }
}

} // namespace ps
