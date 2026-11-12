#include "compress/Compress.h"

#include "adb/AdbClient.h"
#include "nuke/Nuker.h"
#include "scan/FileStore.h"
#include "thumbs/ImageOps.h"
#include "transfer/LocalPath.h"
#include "transfer/Manifest.h"
#include "transfer/PhoneIO.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Process.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <stb_image_write.h>

#include <algorithm>
#include <format>
#include <fstream>

namespace ps {

namespace {

constexpr double kMinSavingFraction = 0.20; // plan: only files expected to shrink by >= 20%
constexpr double kKeepIfAbove = 0.85;       // run: discard a result that isn't >= 15% smaller

std::string LowerExt(std::string_view path)
{
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string e(path.substr(dot + 1));
    for (char& c : e)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return e;
}

bool IsVideoExt(const std::string& e) { return e == "mp4" || e == "m4v" || e == "mov" || e == "3gp" || e == "mkv" || e == "avi"; }
bool IsJpegExt(const std::string& e) { return e == "jpg" || e == "jpeg"; }

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(LongPath(p), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool WriteFile(const std::filesystem::path& p, std::string_view data)
{
    std::ofstream out(LongPath(p), std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

} // namespace

// ---- Planning ------------------------------------------------------------------

std::map<std::string, MediaInfo, std::less<>> ParseMediaInfoRows(std::string_view text)
{
    std::map<std::string, MediaInfo, std::less<>> out;
    ForEachLine(text, [&](std::string_view line) {
        if (!line.starts_with("Row: "))
            return;
        const auto data = line.find(" _data=");
        if (data == std::string_view::npos)
            return;
        // Parse known trailing fields from the right (paths may contain ", ").
        std::string_view rest = line.substr(data + 7);
        auto take = [&](std::string_view key) -> std::optional<int64_t> {
            const auto p = rest.rfind(std::string(", ") + std::string(key) + "=");
            if (p == std::string_view::npos)
                return std::nullopt;
            const auto v = ParseInt<int64_t>(Trim(rest.substr(p + key.size() + 3)));
            rest = rest.substr(0, p);
            return v;
        };
        take("_size");
        MediaInfo info;
        info.height = static_cast<int>(take("height").value_or(0));
        info.width = static_cast<int>(take("width").value_or(0));
        info.durationMs = take("duration").value_or(0);
        if (!rest.empty() && rest != "NULL")
            out[std::string(rest)] = info;
    });
    return out;
}

int TargetVideoKbps(int width, int height)
{
    const int longEdge = std::min(std::max(width, height), 1920);
    if (longEdge <= 854)
        return 1200;
    if (longEdge <= 1280)
        return 2500;
    return 4500;
}

std::optional<uint64_t> EstimateVideo(uint64_t size, const MediaInfo& info)
{
    if (info.durationMs < 3000 || size < 20'000'000 || info.width <= 0 || info.height <= 0)
        return std::nullopt;
    const double seconds = static_cast<double>(info.durationMs) / 1000.0;
    const double kbps = TargetVideoKbps(info.width, info.height) + 128.0;
    // MediaStore doesn't tell us the codec. HEVC needs roughly half the bitrate of H.264 for the
    // same picture, so only re-encode when the source runs well above our target; otherwise the
    // H.264 output can come out BIGGER (measured: 720p HEVC at 5.2 Mbps, 87.5 -> 109.5 MB).
    const double sourceKbps = static_cast<double>(size) * 8.0 / 1000.0 / seconds;
    if (sourceKbps < kbps * 2.5)
        return std::nullopt;
    const auto est = static_cast<uint64_t>(kbps * 1000.0 / 8.0 * seconds * 1.10); // +10% container/VBR headroom
    if (static_cast<double>(est) > static_cast<double>(size) * (1.0 - kMinSavingFraction))
        return std::nullopt; // already efficient (e.g. Samsung HEVC 720p)
    return est;
}

std::optional<uint64_t> EstimatePhoto(uint64_t size, const MediaInfo& info)
{
    if (size < 1'500'000 || info.width <= 0 || info.height <= 0)
        return std::nullopt;
    // JPEG quality 85 on camera photos lands around 2.2 bits per pixel.
    const auto est = static_cast<uint64_t>(static_cast<double>(info.width) * info.height * 2.2 / 8.0);
    if (static_cast<double>(est) > static_cast<double>(size) * (1.0 - kMinSavingFraction))
        return std::nullopt;
    return est;
}

std::vector<CompressCandidate> FindCompressCandidates(const FileStore& files, const VerifiedIndex& verified,
    const std::map<std::string, MediaInfo, std::less<>>& media)
{
    std::vector<CompressCandidate> out;
    const auto& f = files.Files();
    for (uint32_t i = 0; i < f.size(); ++i) {
        const FileEntry& e = f[i];
        if (e.IsProtected() || (e.flags & FileFlag_Keep))
            continue; // KEEP means "leave it exactly as it is"
        const std::string ext = LowerExt(e.path);
        const bool video = IsVideoExt(ext), photo = IsJpegExt(ext);
        if (!video && !photo)
            continue;
        const auto it = media.find(e.path);
        if (it == media.end())
            continue;
        const auto est = video ? EstimateVideo(e.size, it->second) : EstimatePhoto(e.size, it->second);
        if (!est)
            continue;
        CompressCandidate c;
        c.file = i;
        c.path = e.path;
        c.kind = video ? MediaKind::Video : MediaKind::Photo;
        c.size = e.size;
        c.mtime = e.mtime;
        c.info = it->second;
        c.estimatedSize = *est;
        c.saved = verified.IsVerified(e.path, e.size, e.mtime);
        out.push_back(std::move(c));
    }
    std::ranges::sort(out, [](const CompressCandidate& a, const CompressCandidate& b) { return a.EstimatedSaving() > b.EstimatedSaving(); });
    return out;
}

std::vector<std::string> VideoArgs(const std::filesystem::path& in, const std::filesystem::path& out, const MediaInfo& info, bool qsv)
{
    const int kbps = TargetVideoKbps(info.width, info.height);
    std::vector<std::string> a = { "-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", "-y", "-i", Narrow(in.wstring()),
        "-map", "0:v:0", "-map", "0:a:0?", "-map_metadata", "0" };
    if (std::max(info.width, info.height) > 1920) // 4K -> 1080p long edge, aspect kept, even dimensions
        a.insert(a.end(), { "-vf", "scale='if(gte(iw,ih),1920,-2)':'if(gte(iw,ih),-2,1920)'" });
    if (qsv)
        a.insert(a.end(), { "-c:v", "h264_qsv", "-preset", "medium", "-b:v", std::format("{}k", kbps), "-maxrate",
                              std::format("{}k", kbps * 3 / 2), "-bufsize", std::format("{}k", kbps * 2) });
    else
        a.insert(a.end(), { "-c:v", "libx264", "-preset", "medium", "-crf", "26", "-maxrate", std::format("{}k", kbps), "-bufsize",
                              std::format("{}k", kbps * 2), "-pix_fmt", "yuv420p" });
    a.insert(a.end(), { "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart+use_metadata_tags", "-f", "mp4", Narrow(out.wstring()) });
    return a;
}

std::string SpliceExif(std::string_view original, std::string_view encoded)
{
    if (encoded.size() < 2 || static_cast<uint8_t>(encoded[0]) != 0xFF || static_cast<uint8_t>(encoded[1]) != 0xD8)
        return std::string(encoded);
    // Find the Exif APP1 segment in the original (before SOS).
    size_t p = 2;
    while (p + 4 <= original.size()) {
        if (static_cast<uint8_t>(original[p]) != 0xFF)
            break;
        const uint8_t marker = static_cast<uint8_t>(original[p + 1]);
        if (marker == 0xDA || marker == 0xD9)
            break;
        const size_t len = (static_cast<size_t>(static_cast<uint8_t>(original[p + 2])) << 8) | static_cast<uint8_t>(original[p + 3]);
        if (len < 2 || p + 2 + len > original.size())
            break;
        if (marker == 0xE1 && original.substr(p + 4, 6) == std::string_view("Exif\0\0", 6)) {
            std::string out;
            out.reserve(encoded.size() + len + 2);
            out.append(encoded.substr(0, 2));          // SOI
            out.append(original.substr(p, len + 2));  // APP1 Exif
            out.append(encoded.substr(2));
            return out;
        }
        p += 2 + len;
    }
    return std::string(encoded); // no EXIF to keep
}

std::optional<std::string> CompressJpeg(std::string_view original, int quality)
{
    const auto img = DecodeImage({ reinterpret_cast<const uint8_t*>(original.data()), original.size() });
    if (!img)
        return std::nullopt;
    std::string encoded;
    auto write = [](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size); };
    // Pixels stay in stored orientation; the copied EXIF keeps the orientation tag, so it still displays upright.
    if (!stbi_write_jpg_to_func(write, &encoded, img->w, img->h, 4, img->rgba.data(), quality))
        return std::nullopt;
    return SpliceExif(original, encoded);
}

std::string CompressedPhonePath(std::string_view original, MediaKind kind)
{
    std::string p(original);
    if (kind == MediaKind::Video && LowerExt(p) != "mp4") {
        const auto dot = p.rfind('.');
        p = p.substr(0, dot) + ".mp4";
    }
    return p;
}

bool ProbeQsv(const std::filesystem::path& ffmpeg, const std::filesystem::path& tmpDir)
{
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const auto out = tmpDir / L"qsv_probe.mp4";
    const std::vector<std::string> args = { "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=black:s=320x240:d=0.2",
        "-c:v", "h264_qsv", "-b:v", "500k", Narrow(out.wstring()) };
    const auto r = RunProcess(ffmpeg, args, { .timeout = 30s, .logName = "ffmpeg" });
    const bool ok = r.ok() && std::filesystem::file_size(out, ec) > 0;
    std::filesystem::remove(out, ec);
    return ok;
}

// ---- Running ---------------------------------------------------------------------

void RunCompress(const AdbClient& adb, const CompressTask& t, CompressProgress& p, std::stop_token st)
{
    {
        std::lock_guard l(p.mutex);
        p.total = t.items.size();
    }
    std::error_code ec;
    std::filesystem::create_directories(t.tmpDir, ec);
    for (size_t idx = 0; idx < t.items.size() && !st.stop_requested(); ++idx) {
        const CompressCandidate& c = t.items[idx];
        CompressItemResult res;
        res.path = c.path;
        res.before = c.size;
        auto step = [&](const char* s, float frac = 0) {
            std::lock_guard l(p.mutex);
            p.index = idx;
            p.current = c.path.substr(c.path.rfind('/') + 1);
            p.step = s;
            p.fileFraction = frac;
        };
        const bool video = c.kind == MediaKind::Video;
        const auto inTmp = t.tmpDir / (video ? L"compress_in" : L"compress_in.jpg");
        const auto outTmp = t.tmpDir / (video ? L"compress_out.mp4" : L"compress_out.jpg");
        struct Cleanup {
            std::filesystem::path a, b;
            ~Cleanup()
            {
                std::error_code e;
                std::filesystem::remove(a, e);
                std::filesystem::remove(b, e);
            }
        } cleanup{ inTmp, outTmp };

        auto finish = [&](std::string status, bool ok) {
            res.status = std::move(status);
            res.ok = ok;
            LogInfo("Compress {}: {} ({} -> {} bytes)", c.path, res.status, res.before, res.after);
            std::lock_guard l(p.mutex);
            p.results.push_back(res);
        };

        // 1. Copy the original to the PC.
        step("copying from the phone");
        const auto pull = PullToFile(adb, t.serial, c.path, inTmp, st,
            [&](uint64_t b) { step("copying from the phone", c.size ? static_cast<float>(b) / static_cast<float>(c.size) * 0.3f : 0.0f); });
        if (!pull.ok || pull.bytes != c.size) {
            finish(pull.ok ? "The file changed on the phone; skipped" : pull.error, false);
            continue;
        }

        // 2. Encode.
        step("compressing", 0.3f);
        if (video) {
            const auto args = VideoArgs(inTmp, outTmp, c.info, t.useQsv);
            std::string pending;
            auto r = RunProcess(t.ffmpeg, args, { .timeout = std::chrono::hours(3), .cancel = st, .logName = "ffmpeg" }, [&](std::string_view chunk) {
                // -progress pipe:1 prints "out_time_us=12345678" lines.
                pending.append(chunk);
                for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1)) {
                    const std::string_view line(pending.data(), nl);
                    if (line.starts_with("out_time_us=") && c.info.durationMs > 0)
                        if (auto us = ParseInt<int64_t>(Trim(line.substr(12))))
                            step("compressing", 0.3f + 0.6f * std::clamp(static_cast<float>(*us) / (c.info.durationMs * 1000.0f), 0.0f, 1.0f));
                }
            });
            if (!r.ok() && t.useQsv && !st.stop_requested()) {
                LogWarn("Quick Sync encode failed, retrying with libx264: {}", r.err);
                r = RunProcess(t.ffmpeg, VideoArgs(inTmp, outTmp, c.info, false), { .timeout = std::chrono::hours(3), .cancel = st, .logName = "ffmpeg" });
            }
            if (!r.ok()) {
                finish(st.stop_requested() ? "cancelled" : "ffmpeg failed: " + r.err.substr(0, 200), false);
                continue;
            }
        } else {
            const auto jpg = CompressJpeg(ReadFile(inTmp), 85);
            if (!jpg || !WriteFile(outTmp, *jpg)) {
                finish("Couldn't re-encode this photo", false);
                continue;
            }
        }
        res.after = std::filesystem::file_size(outTmp, ec);
        if (ec || res.after == 0 || static_cast<double>(res.after) > static_cast<double>(c.size) * kKeepIfAbove) {
            res.after = c.size;
            finish("already efficient (kept the original)", true);
            continue;
        }
        const auto outMd5 = Md5File(LongPath(outTmp));
        if (!outMd5) {
            finish("Couldn't read the compressed file", false);
            continue;
        }

        // 3. PC copy of the compressed file (PhoneAndPc / PcOnly).
        const std::string newPhonePath = CompressedPhonePath(c.path, c.kind);
        if (t.outcome != CompressOutcome::PhoneOnly) {
            step("saving to the PC", 0.92f);
            std::string prefix;
            std::string_view rel = newPhonePath;
            for (size_t v = 0; v < t.volumeRoots.size(); ++v)
                if (newPhonePath.starts_with(t.volumeRoots[v])) {
                    rel = std::string_view(newPhonePath).substr(t.volumeRoots[v].size());
                    prefix = v > 0 && v < t.volumeLabels.size() ? t.volumeLabels[v] : std::string();
                }
            auto dest = NextFreeName(t.pcFolder / MapToLocal(prefix, rel, true));
            std::filesystem::create_directories(LongPath(dest.parent_path()), ec);
            std::filesystem::copy_file(LongPath(outTmp), LongPath(dest), std::filesystem::copy_options::overwrite_existing, ec);
            if (ec || Md5File(LongPath(dest)) != outMd5) {
                finish("Couldn't save the compressed copy on the PC: " + ec.message(), false);
                continue;
            }
            SetFileMtime(dest, c.mtime);
        }

        // 4. Phone side.
        step(t.outcome == CompressOutcome::PcOnly ? "removing from the phone" : "replacing on the phone", 0.95f);
        if (t.outcome == CompressOutcome::PcOnly) {
            const auto gone = DeleteFiles(adb, t.serial, { c.path }, t.tmpDir, st);
            CleanMediaStore(adb, t.serial, gone, st);
            if (gone.empty()) {
                finish("Saved on the PC, but couldn't delete the original from the phone", false);
                continue;
            }
            res.newPath.clear();
            finish("compressed copy saved on the PC; original removed from the phone", true);
            continue;
        }
        if (newPhonePath != c.path) {
            // Video becomes .mp4: don't overwrite an unrelated file with that name.
            const auto exists = adb.Shell(t.serial, ShellCommand("ls").arg("-d").arg(newPhonePath).raw("2>/dev/null"), { .timeout = 20s, .cancel = st });
            if (!Trim(exists.out).empty()) {
                finish("A file named " + newPhonePath.substr(newPhonePath.rfind('/') + 1) + " already exists; skipped", false);
                continue;
            }
        }
        const std::string err = PushVerified(adb, t.serial, outTmp, newPhonePath, *outMd5, c.mtime, st);
        if (!err.empty()) {
            finish(err, false);
            continue;
        }
        if (newPhonePath != c.path) {
            const auto gone = DeleteFiles(adb, t.serial, { c.path }, t.tmpDir, st);
            CleanMediaStore(adb, t.serial, gone, st);
        }
        MediaScan(adb, t.serial, { newPhonePath }, st);
        res.newPath = newPhonePath;
        finish(t.outcome == CompressOutcome::PhoneAndPc ? "compressed on the phone + copy on the PC" : "compressed on the phone", true);
    }
    std::lock_guard l(p.mutex);
    p.done = true;
}

} // namespace ps
