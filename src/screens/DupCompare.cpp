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
constexpr uint64_t kMaxPullBytes = 4'000'000'000ull;

std::string LowerExt(std::string_view name)
{
    const size_t dot = name.rfind('.');
    std::string ext = dot == std::string_view::npos ? std::string() : std::string(name.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Media we hand to Windows' default app. Never anything that could run (exe, js, bat...).
bool SafeToOpen(std::string_view name)
{
    static constexpr std::array<std::string_view, 30> kOk = { "jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "bmp", "tif", "tiff",
        "mp4", "m4v", "mkv", "mov", "3gp", "webm", "avi", "wmv", "ts", "mp3", "m4a", "aac", "wav", "ogg", "oga", "opus", "flac", "wma",
        "amr", "mid" };
    const std::string ext = LowerExt(name);
    return ext.size() <= 5 && std::ranges::find(kOk, ext) != kOk.end();
}

std::string Clock(uint32_t ms)
{
    const uint32_t s = ms / 1000;
    return std::format("{}:{:02}", s / 60, s % 60);
}

std::string Folder(std::string_view path)
{
    std::string folder(path.substr(0, path.rfind('/')));
    if (folder.starts_with("/storage/emulated/0/"))
        return "Internal \xC2\xB7 " + folder.substr(20);
    if (folder == "/storage/emulated/0")
        return "Internal storage";
    if (folder.starts_with("/storage/")) {
        const size_t slash = folder.find('/', 9);
        return slash == std::string::npos ? std::string("SD card") : "SD card \xC2\xB7 " + folder.substr(slash + 1);
    }
    return folder;
}

} // namespace

DupCompare::DupCompare()
{
    std::error_code ec;
    m_tempDir = std::filesystem::temp_directory_path(ec) / L"PhoneSweep-preview";
    std::filesystem::remove_all(m_tempDir, ec); // leftovers of an earlier session
}

DupCompare::~DupCompare() { ReleaseFiles(); }

void DupCompare::Open(int group)
{
    m_group = group;
    m_justOpened = true;
}

void DupCompare::Close()
{
    m_group = -1;
    ReleaseFiles();
}

void DupCompare::ReleaseFiles()
{
    m_audio.Close();
    for (auto& [path, p] : m_pulls)
        if (p->job)
            p->job->Cancel();
    m_pulls.clear();
    m_text.clear();
    std::error_code ec;
    std::filesystem::remove_all(m_tempDir, ec); // a file still open in a player just stays until next time
}

DupCompare::TextFetch& DupCompare::Text(AppContext& ctx, const FileEntry& e)
{
    auto [it, inserted] = m_text.try_emplace(e.path);
    if (inserted) {
        it->second = std::make_shared<TextFetch>();
        auto state = it->second;
        if (const auto local = ctx.verified.IsVerified(e.path, e.size, e.mtime) ? ctx.verified.LocalFile(e.path) : std::nullopt) {
            // Saved on the PC: read the backup instead of the phone.
            std::string bytes(std::min<uint64_t>(e.size, kTextPreviewBytes), '\0');
            FILE* f = nullptr;
            _wfopen_s(&f, local->c_str(), L"rb");
            const size_t got = f ? fread(bytes.data(), 1, bytes.size(), f) : 0;
            if (f)
                fclose(f);
            bytes.resize(got);
            state->text = DecodeTextPreview(bytes);
            state->unreadable = !f;
            state->done = true;
        } else if (ctx.adb && ctx.ActiveConnected()) {
            auto adb = ctx.adb;
            const std::string serial = ctx.activeSerial, path = e.path;
            ctx.jobs.Submit("Loading preview", JobPriority::Normal, [adb, serial, path, state](Job&, std::stop_token st) {
                const auto bytes = FetchFileHead(*adb, serial, path, kTextPreviewBytes, st);
                std::lock_guard l(state->mutex);
                if (bytes)
                    state->text = DecodeTextPreview(*bytes);
                state->unreadable = !bytes;
                state->done = true;
            });
        } else {
            state->unreadable = true;
            state->done = true;
        }
    }
    return *it->second;
}

void DupCompare::RequestLocal(AppContext& ctx, const FileEntry& e, After after)
{
    if (auto it = m_pulls.find(e.path); it != m_pulls.end()) {
        std::lock_guard l(it->second->mutex);
        if (!it->second->done)
            return; // already on its way
        if (it->second->ok) {
            it->second->after = after;
            it->second->handled = false; // run the action again
            return;
        }
    }
    auto pull = std::make_shared<Pull>();
    pull->after = after;
    pull->total = e.size;
    m_pulls[e.path] = pull;

    if (const auto local = ctx.verified.IsVerified(e.path, e.size, e.mtime) ? ctx.verified.LocalFile(e.path) : std::nullopt) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(*local, ec)) {
            pull->local = *local;
            pull->ok = pull->done = true;
            return;
        }
    }
    if (!ctx.adb || !ctx.ActiveConnected() || e.size > kMaxPullBytes) {
        pull->done = true;
        pull->error = e.size > kMaxPullBytes ? "Too big to preview" : "Phone not connected";
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_tempDir, ec);
    const std::string ext = LowerExt(e.Name());
    const auto local = m_tempDir / Widen(std::format("{:016x}.{}", std::hash<std::string>{}(e.path), ext));
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial, path = e.path;
    pull->job = ctx.jobs.Submit("Copying preview to the PC", JobPriority::Normal, [adb, serial, path, local, pull](Job& job, std::stop_token st) {
        const auto r = PullToFile(*adb, serial, path, local, st, [&](uint64_t b) {
            std::lock_guard l(pull->mutex);
            pull->bytes = b;
            if (pull->total)
                job.SetProgress(static_cast<float>(b) / static_cast<float>(pull->total));
        });
        std::lock_guard l(pull->mutex);
        pull->ok = r.ok;
        pull->error = r.ok ? std::string() : r.error;
        pull->local = local;
        pull->done = true;
    });
}

void DupCompare::PumpPulls(AppContext& ctx)
{
    for (auto& [path, p] : m_pulls) {
        std::filesystem::path local;
        After after;
        {
            std::lock_guard l(p->mutex);
            if (!p->done || p->handled)
                continue;
            p->handled = true;
            if (!p->ok) {
                ui::Toast(std::format("Couldn't copy the file: {}", p->error), ui::ToastKind::Error);
                continue;
            }
            local = p->local;
            after = p->after;
        }
        if (after == After::Play) {
            if (m_audio.Play(local, path))
                continue;
            // No Windows codec (opus / ogg / amr on many PCs): play a WAV made by ffmpeg instead.
            const bool converted = local.extension() == L".wav" && local.parent_path() == m_tempDir;
            if (ctx.ffmpeg && !converted) {
                const auto wav = m_tempDir / Widen(std::format("{:016x}.wav", std::hash<std::string>{}(path)));
                {
                    std::lock_guard l(p->mutex);
                    p->done = p->ok = p->handled = false;
                    p->converting = true;
                }
                std::error_code ec;
                std::filesystem::create_directories(m_tempDir, ec);
                const auto ffmpeg = *ctx.ffmpeg;
                auto pull = p;
                p->job = ctx.jobs.Submit("Preparing audio preview", JobPriority::Normal, [ffmpeg, local, wav, pull](Job&, std::stop_token st) {
                    const std::vector<std::string> args = { "-hide_banner", "-loglevel", "error", "-y", "-i", Narrow(local.wstring()), "-vn", "-t",
                        "1200", "-ac", "2", "-ar", "44100", "-f", "wav", Narrow(wav.wstring()) };
                    const auto r = RunProcess(ffmpeg, args, { .timeout = 120s, .cancel = st, .logName = "ffmpeg" });
                    std::lock_guard l(pull->mutex);
                    pull->ok = r.ok();
                    pull->error = r.ok() ? std::string() : "ffmpeg couldn't read it";
                    pull->local = wav;
                    pull->converting = false;
                    pull->done = true;
                });
                continue;
            }
            ui::Toast("Windows can't play this format here; opening it in your player instead", ui::ToastKind::Info);
            ctx.platform.OpenInExplorer(local);
        } else {
            ctx.platform.OpenInExplorer(local); // ShellExecute "open": the default app for this media type
        }
    }
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
    PumpPulls(ctx);
    if (m_audio.TakeFailure()) {
        const auto file = m_audio.File();
        m_audio.Close();
        ui::Toast("Windows can't play this format here; opening it in your player instead", ui::ToastKind::Info);
        ctx.platform.OpenInExplorer(file);
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
        if (ui::GhostButton(ICON_XMARK "##closecmp", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            Close();
            return;
        }
    }
    {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText("These files are byte-for-byte identical (same MD5). Choose which copies stay. "
                    "Nothing is deleted here: the Delete button in the list does that, after asking.");
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
        if (tick && kept < n)
            ui::ColoredText(Danger, std::format("Keeps {}  \xC2\xB7  deletes {}  \xC2\xB7  frees {}", kept, n - kept, ui::Bytes(grp.Reclaimable())));
        else
            ui::ColoredText(Accent, "Keeps every copy (this group is skipped)");
    }
    ImGui::SameLine();
    {
        const char* keepAll = ICON_CHECK "  Keep all copies";
        const char* done = "Done";
        const float w = ui::TextWidth(keepAll) + ui::TextWidth(done) + S(80);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
        if (ui::SecondaryButton(keepAll, {}, tick))
            tick = false;
        ImGui::SetItemTooltip("Skip this group: nothing in it is deleted.");
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
    if (go != m_group) {
        m_audio.Close();
        m_group = go;
    }
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
        DrawPreview(ctx, *e, a, b);
    else
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, U32(Border, 0.4f), S(6));
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
        ui::DimText(Folder(df.path));
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

void DupCompare::DrawPreview(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = b.x - a.x;
    PreviewKind kind = PreviewKindFor(e);
    const float buttonsH = (kind == PreviewKind::Image || kind == PreviewKind::Video || kind == PreviewKind::Audio) ? ImGui::GetFrameHeight() + S(8) : 0.0f;
    const ImVec2 pb(b.x, b.y - buttonsH);

    if (kind == PreviewKind::Other) {
        // Unknown type: peek at it and show it as text when it is text.
        TextFetch& t = Text(ctx, e);
        std::lock_guard l(t.mutex);
        if (t.done && t.text)
            kind = PreviewKind::Text;
    }

    switch (kind) {
    case PreviewKind::Image:
    case PreviewKind::Video: {
        ThumbView view = ctx.thumbs.Request(e, ThumbVariant::Preview);
        if (view.state != ThumbState::Ready)
            if (const ThumbView grid = ctx.thumbs.Peek(e, ThumbVariant::Grid); grid.state == ThumbState::Ready)
                view = grid;
        DrawThumbArea(dl, a, pb, e, view, false, S(6), 56.0f);
        if (view.state == ThumbState::Unsupported) {
            Font f(GetFonts().ui, FontSmall);
            const char* msg = e.category == Category::Videos ? "Install ffmpeg for video frames" : "No preview for this format";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, pb.y - ts.y - S(8)), U32(TextDim), msg);
        }
        break;
    }
    case PreviewKind::Audio: {
        dl->AddRectFilled(a, pb, U32(ui::CategoryColor(static_cast<uint8_t>(Category::Music)), 0.08f), S(6));
        const bool loaded = m_audio.Key() == e.path;
        const bool playing = loaded && m_audio.IsPlaying();
        {
            Font f(GetFonts().ui, 56.0f);
            const char* icon = ICON_MUSIC;
            const ImVec2 ts = ImGui::CalcTextSize(icon);
            const float pulse = playing ? 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f) : 0.85f;
            dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, (a.y + pb.y - ts.y) * 0.5f - S(10)),
                U32(ui::CategoryColor(static_cast<uint8_t>(Category::Music)), pulse), icon);
        }
        if (loaded) {
            // Progress line + time; click to seek.
            const uint32_t pos = m_audio.PositionMs(), len = m_audio.LengthMs();
            const float y = pb.y - S(26);
            const ImVec2 l0(a.x + S(14), y), l1(pb.x - S(14), y + S(4));
            dl->AddRectFilled(l0, l1, U32(Border), S(2));
            if (len)
                dl->AddRectFilled(l0, ImVec2(l0.x + (l1.x - l0.x) * std::min(1.0f, static_cast<float>(pos) / static_cast<float>(len)), l1.y), U32(Accent), S(2));
            ImGui::SetCursorScreenPos(ImVec2(l0.x, y - S(6)));
            if (ImGui::InvisibleButton("##seek", ImVec2(l1.x - l0.x, S(16))) && len)
                m_audio.SeekMs(static_cast<uint32_t>((ImGui::GetIO().MousePos.x - l0.x) / (l1.x - l0.x) * static_cast<float>(len)));
            Font f(GetFonts().ui, FontSmall);
            const std::string t = std::format("{} / {}", Clock(pos), Clock(len));
            dl->AddText(ImVec2(l0.x, y + S(8)), U32(TextDim), t.c_str());
        }
        break;
    }
    case PreviewKind::Text: {
        TextFetch& t = Text(ctx, e);
        std::lock_guard l(t.mutex);
        dl->AddRectFilled(a, pb, U32(Background), S(6));
        if (!t.done) {
            ImGui::SetCursorScreenPos(ImVec2(a.x + w * 0.5f - S(10), (a.y + pb.y) * 0.5f - S(10)));
            ui::Spinner(S(10), S(2.5f), Accent);
        } else if (!t.text) {
            Font f(GetFonts().ui, FontSmall);
            const char* msg = t.unreadable ? "Couldn't read this file" : "Not a text file";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, (a.y + pb.y - ts.y) * 0.5f), U32(TextDim), msg);
        } else {
            ImGui::SetCursorScreenPos(a);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Background);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(6)));
            ImGui::BeginChild("##text", ImVec2(w, pb.y - a.y), ImGuiChildFlags_AlwaysUseWindowPadding);
            {
                Font f(GetFonts().mono, FontSmall);
                ImGui::PushTextWrapPos(0.0f);
                if (t.text->empty())
                    ui::DimText("(empty file)");
                else
                    ImGui::TextUnformatted(t.text->data(), t.text->data() + t.text->size());
                ImGui::PopTextWrapPos();
                if (e.size > kTextPreviewBytes)
                    ui::DimText(std::format("\xE2\x80\xA6 first {} of {} shown", ui::Bytes(kTextPreviewBytes), ui::Bytes(e.size)));
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        break;
    }
    case PreviewKind::Other: {
        DrawThumbArea(dl, a, pb, e, ThumbView{}, false, S(6), 56.0f);
        TextFetch& t = Text(ctx, e);
        std::lock_guard l(t.mutex);
        Font f(GetFonts().ui, FontSmall);
        const char* msg = t.done ? "No preview for this type of file" : "Looking inside\xE2\x80\xA6";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, pb.y - ts.y - S(10)), U32(TextDim), msg);
        break;
    }
    }

    if (buttonsH > 0) {
        ImGui::SetCursorScreenPos(ImVec2(a.x, pb.y + S(8)));
        DrawMediaButtons(ctx, e, w);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
}

void DupCompare::DrawMediaButtons(AppContext& ctx, const FileEntry& e, float width)
{
    std::shared_ptr<Pull> pull;
    if (auto it = m_pulls.find(e.path); it != m_pulls.end())
        pull = it->second;
    bool busy = false;
    float progress = 0;
    After busyAfter = After::OpenOnPc;
    bool converting = false;
    if (pull) {
        std::lock_guard l(pull->mutex);
        busy = !pull->done;
        converting = pull->converting;
        busyAfter = pull->after;
        progress = pull->total ? static_cast<float>(pull->bytes) / static_cast<float>(pull->total) : 0.0f;
    }
    const bool canOpen = SafeToOpen(e.Name());

    if (e.category == Category::Music) {
        const bool playing = m_audio.Key() == e.path && m_audio.IsPlaying();
        if (busy && busyAfter == After::Play) {
            ui::SecondaryButton(converting ? "Preparing…##play" : std::format("Loading {:.0f}%##play", progress * 100.0f).c_str(), ImVec2(S(110), 0), false);
        } else if (playing) {
            if (ui::PrimaryButton(ICON_STOP "  Stop", ImVec2(S(110), 0)))
                m_audio.Close();
        } else if (ui::PrimaryButton(ICON_PLAY "  Play", ImVec2(S(110), 0), canOpen)) {
            m_audio.Close();
            RequestLocal(ctx, e, After::Play);
        }
        ImGui::SameLine(0, S(6));
    }
    const std::string openLabel = busy && busyAfter == After::OpenOnPc ? std::format("Copying {:.0f}%##open", progress * 100.0f)
        : e.category == Category::Videos                               ? std::string(ICON_PLAY "  Play on PC")
                                                                       : std::string(ICON_EXTERNAL "  Open on PC");
    if (ui::SecondaryButton(openLabel.c_str(), {}, canOpen && !busy))
        RequestLocal(ctx, e, After::OpenOnPc);
    if (canOpen)
        ImGui::SetItemTooltip("%s", ctx.verified.IsVerified(e.path, e.size, e.mtime) ? "Opens your saved copy in the default app."
                                                                                     : "Copies it to a temporary folder and opens it in the default app.");
    (void)width;
}

} // namespace ps
