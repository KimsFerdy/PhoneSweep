#include "screens/PreviewKit.h"

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

std::filesystem::path PreviewRoot()
{
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / L"PhoneSweep-preview";
}

} // namespace

std::string FolderCaption(std::string_view path)
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

PreviewKit::PreviewKit()
{
    static int s_instances = 0;
    std::error_code ec;
    if (s_instances == 0)
        std::filesystem::remove_all(PreviewRoot(), ec); // leftovers of an earlier session
    m_tempDir = PreviewRoot() / std::to_wstring(++s_instances); // each window its own folder
}

PreviewKit::~PreviewKit() { Release(); }

void PreviewKit::Release()
{
    m_audio.Close();
    for (auto& [path, p] : m_pulls)
        if (p->job)
            p->job->Cancel();
    m_pulls.clear();
    m_text.clear();
    ResetZoom();
    m_zoomKey.clear();
    std::error_code ec;
    std::filesystem::remove_all(m_tempDir, ec); // a file still open in a player just stays until next time
}

void PreviewKit::Pump(AppContext& ctx)
{
    PumpPulls(ctx);
    if (m_audio.TakeFailure()) {
        const auto file = m_audio.File();
        m_audio.Close();
        ui::Toast("Windows can't play this format here; opening it in your player instead", ui::ToastKind::Info);
        ctx.platform.OpenInExplorer(file);
    }
}

bool PreviewKit::CanOpenOnPc(const FileEntry& e) { return SafeToOpen(e.Name()); }

void PreviewKit::OpenOnPc(AppContext& ctx, const FileEntry& e)
{
    if (CanOpenOnPc(e))
        RequestLocal(ctx, e, After::OpenOnPc);
}

void PreviewKit::Draw(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b, bool zoomable)
{
    DrawPreview(ctx, e, a, b, zoomable);
}

PreviewKit::TextFetch& PreviewKit::Text(AppContext& ctx, const FileEntry& e)
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

void PreviewKit::RequestLocal(AppContext& ctx, const FileEntry& e, After after)
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

void PreviewKit::PumpPulls(AppContext& ctx)
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

void PreviewKit::DrawZoomable(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (m_zoomKey != e.path) {
        m_zoomKey = e.path;
        ResetZoom();
    }
    // Sharpest available: full (2048 px) > preview (640) > grid (256).
    ThumbView view = ctx.thumbs.Request(e, ThumbVariant::Full);
    bool sharp = view.state == ThumbState::Ready;
    if (!sharp) {
        const ThumbView pv = ctx.thumbs.Request(e, ThumbVariant::Preview);
        view = pv.state == ThumbState::Ready ? pv : ctx.thumbs.Peek(e, ThumbVariant::Grid);
        if (view.state != ThumbState::Ready && (pv.state == ThumbState::Unsupported || pv.state == ThumbState::Failed))
            view = pv;
    }
    if (view.state != ThumbState::Ready || !view.texture || view.w <= 0 || view.h <= 0) {
        DrawThumbArea(dl, a, b, e, view.state == ThumbState::None ? ThumbView{ ThumbState::Loading } : view, false, S(6), 72.0f);
        return;
    }

    // Input: wheel zooms around the mouse, drag pans, double-click resets.
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##zoom", ImVec2(b.x - a.x, b.y - a.y));
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float old = m_zoom;
            m_zoom = std::clamp(m_zoom * std::pow(1.2f, wheel), 1.0f, 10.0f);
            const ImVec2 m = ImGui::GetIO().MousePos;
            // Keep the point under the mouse where it is.
            m_pan.x = (m_pan.x - (m.x - c.x)) * (m_zoom / old) + (m.x - c.x);
            m_pan.y = (m_pan.y - (m.y - c.y)) * (m_zoom / old) + (m.y - c.y);
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            ResetZoom();
        ImGui::SetMouseCursor(m_zoom > 1.0f ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        m_pan.x += ImGui::GetIO().MouseDelta.x;
        m_pan.y += ImGui::GetIO().MouseDelta.y;
    }

    // Fit, then scale + pan; never let the picture leave the box entirely.
    const float bw = b.x - a.x, bh = b.y - a.y;
    const float aspect = static_cast<float>(view.w) / static_cast<float>(view.h);
    float w = bw, h = bw / aspect;
    if (h > bh) {
        h = bh;
        w = bh * aspect;
    }
    w *= m_zoom;
    h *= m_zoom;
    const float maxX = std::max(0.0f, (w - bw) * 0.5f), maxY = std::max(0.0f, (h - bh) * 0.5f);
    m_pan.x = std::clamp(m_pan.x, -maxX, maxX);
    m_pan.y = std::clamp(m_pan.y, -maxY, maxY);
    const ImVec2 p0(c.x - w * 0.5f + m_pan.x, c.y - h * 0.5f + m_pan.y);
    dl->AddRectFilled(a, b, U32(Background), S(6));
    dl->PushClipRect(a, b, true);
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(view.texture)), p0, ImVec2(p0.x + w, p0.y + h));
    dl->PopClipRect();

    // Zoom badge.
    Font f(GetFonts().ui, FontSmall);
    const std::string z = m_zoom > 1.001f ? std::format("{:.0f}%", m_zoom * 100.0f) : std::string("Scroll to zoom");
    const ImVec2 ts = ImGui::CalcTextSize(z.c_str());
    const ImVec2 bmin(b.x - ts.x - S(18), b.y - ts.y - S(14));
    dl->AddRectFilled(bmin, ImVec2(b.x - S(8), b.y - S(8)), U32Raw(Hex(0x000000), 0.55f), S(4));
    dl->AddText(ImVec2(bmin.x + S(5), bmin.y + S(3)), U32(theme::Text, sharp ? 1.0f : 0.7f), z.c_str());
}

void PreviewKit::DrawPreview(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b, bool zoomable)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = b.x - a.x;
    PreviewKind kind = PreviewKindFor(e);
    // Media buttons under the preview; the big preview window has its own for photos and videos.
    const bool buttons = kind == PreviewKind::Audio || (!zoomable && (kind == PreviewKind::Image || kind == PreviewKind::Video));
    const float buttonsH = buttons ? ImGui::GetFrameHeight() + S(8) : 0.0f;
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
        if (zoomable) {
            DrawZoomable(ctx, e, a, pb);
            break;
        }
        [[fallthrough]];
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

void PreviewKit::DrawMediaButtons(AppContext& ctx, const FileEntry& e, float width)
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
