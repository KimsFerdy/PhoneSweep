#include "thumbs/ThumbFetch.h"

#include "adb/AdbClient.h"
#include "thumbs/Exif.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <vector>

namespace ps {

namespace {

constexpr size_t kExifProbeBytes = 128 * 1024;
constexpr int kMinEmbeddedEdge = 160; // smaller embedded thumbnails look mushy in the grid

std::span<const uint8_t> Bytes(const std::string& s) { return { reinterpret_cast<const uint8_t*>(s.data()), s.size() }; }

// adb failure -> how the cache should treat it.
ThumbOutcome Classify(const AdbResult& r)
{
    if (r.status == AdbResult::Status::Cancelled)
        return ThumbOutcome::Cancelled;
    if (r.status != AdbResult::Status::Ok)
        return ThumbOutcome::Transient;
    const auto& e = r.err;
    if (e.find("No such file") != std::string::npos || e.find("Permission denied") != std::string::npos
        || e.find("Is a directory") != std::string::npos)
        return ThumbOutcome::Failed;
    return ThumbOutcome::Transient; // "device offline", "no devices", adb server hiccups
}

std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return s;
}

} // namespace

uint64_t ThumbKey(std::string_view serial, std::string_view path, uint64_t size, int64_t mtime, ThumbVariant v)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    const char sep = 0;
    mix(serial.data(), serial.size());
    mix(&sep, 1);
    mix(path.data(), path.size());
    mix(&sep, 1);
    mix(&size, sizeof(size));
    mix(&mtime, sizeof(mtime));
    const auto vv = static_cast<uint8_t>(v);
    mix(&vv, 1);
    return h;
}

ThumbFetcher::ThumbFetcher(std::shared_ptr<const AdbClient> adb, ThumbConfig cfg)
    : m_adb(std::move(adb))
    , m_cfg(std::move(cfg))
{
    std::error_code ec;
    std::filesystem::create_directories(m_cfg.cacheDir / L"tmp", ec);
}

std::filesystem::path ThumbFetcher::CachePath(uint64_t key) const
{
    return m_cfg.cacheDir / Widen(std::format("{:016x}.jpg", key));
}

std::optional<Image> ThumbFetcher::FromDisk(uint64_t key) const
{
    const auto path = CachePath(key);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    auto img = DecodeImage(Bytes(data));
    if (img) {
        // Touch for LRU trimming.
        std::error_code ec;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
    }
    return img;
}

void ThumbFetcher::ToDisk(uint64_t key, const Image& img) const
{
    const std::string jpg = EncodeJpeg(img, 85);
    if (jpg.empty())
        return;
    const auto final = CachePath(key);
    auto tmp = final;
    tmp += L".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(jpg.data(), static_cast<std::streamsize>(jpg.size()));
        if (!out)
            return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, final, ec);
}

ThumbResult ThumbFetcher::Produce(const ThumbRequest& req, std::stop_token st) const
{
    const uint64_t key = ThumbKey(req.serial, req.path, req.size, req.mtime, req.variant);
    if (auto img = FromDisk(key)) {
        ThumbResult r;
        r.outcome = ThumbOutcome::Ok;
        r.image = std::move(*img);
        r.fromDisk = true;
        return r;
    }
    if (st.stop_requested())
        return { ThumbOutcome::Cancelled };

    ThumbResult r = req.category == Category::Videos ? FromVideo(req, key, st) : FromDevice(req, st);
    if (r.outcome == ThumbOutcome::Ok)
        ToDisk(key, r.image);
    return r;
}

ThumbResult ThumbFetcher::FromDevice(const ThumbRequest& req, std::stop_token st) const
{
    const auto name = std::string_view(req.path).substr(req.path.rfind('/') + 1);
    if (!IsDecodableImage(name))
        return { ThumbOutcome::Unsupported, {}, false, "format not supported" };
    const int edge = MaxEdge(req.variant);

    // Fast path for grid tiles: phone JPEGs carry a ~500px preview in the first few KB.
    if (IsJpeg(name) && req.variant == ThumbVariant::Grid && req.tryExif && req.size > kExifProbeBytes) {
        const auto r = m_adb->ExecOut(req.serial, ShellCommand("head").arg("-c").arg(std::to_string(kExifProbeBytes)).arg(req.path),
            { .timeout = 20s, .cancel = st });
        if (!r.ok())
            return { Classify(r) };
        const auto bytes = Bytes(r.out);
        const ExifInfo exif = ParseJpegExif(bytes);
        if (exif.thumbLength) {
            if (auto thumb = DecodeImage(bytes.subspan(exif.thumbOffset, exif.thumbLength));
                thumb && std::max(thumb->w, thumb->h) >= kMinEmbeddedEdge) {
                ThumbResult out;
                out.outcome = ThumbOutcome::Ok;
                out.image = FitWithin(ApplyOrientation(std::move(*thumb), exif.orientation), edge);
                out.note = "exif";
                return out;
            }
        }
        // No usable embedded thumbnail: fall through to the full image.
    }

    if (req.size > m_cfg.maxImageBytes)
        return { ThumbOutcome::Unsupported, {}, false, "too large" };

    const auto r = m_adb->ExecOut(req.serial, ShellCommand("cat").arg(req.path), { .timeout = 60s, .cancel = st });
    if (!r.ok())
        return { Classify(r) };
    const auto bytes = Bytes(r.out);
    auto img = DecodeImage(bytes);
    if (!img)
        return { ThumbOutcome::Unsupported, {}, false, "undecodable" };
    const int orientation = IsJpeg(name) ? ParseJpegExif(bytes).orientation : 1;
    ThumbResult out;
    out.outcome = ThumbOutcome::Ok;
    out.image = FitWithin(ApplyOrientation(std::move(*img), orientation), edge);
    return out;
}

ThumbResult ThumbFetcher::FromVideo(const ThumbRequest& req, uint64_t key, std::stop_token st) const
{
    if (!m_cfg.ffmpeg)
        return { ThumbOutcome::Unsupported, {}, false, "no ffmpeg" };
    if (req.size > m_cfg.maxVideoBytes)
        return { ThumbOutcome::Unsupported, {}, false, "video too large" };

    // ffmpeg needs a seekable file (phone MP4s keep the index at the end), so pull it.
    const auto dot = req.path.rfind('.');
    const std::string ext = dot == std::string::npos ? std::string(".mp4") : Lower(req.path.substr(dot));
    const auto tmp = m_cfg.cacheDir / L"tmp" / Widen(std::format("{:016x}{}", key, ext));
    struct TmpGuard {
        std::filesystem::path p;
        ~TmpGuard()
        {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } guard{ tmp };

    const auto pull = m_adb->Pull(req.serial, req.path, tmp, { .timeout = 5min, .cancel = st });
    if (!pull.ok())
        return { Classify(pull) };

    const int edge = MaxEdge(req.variant);
    auto grab = [&](const char* seek) {
        const std::vector<std::string> args = { "-hide_banner", "-loglevel", "error", "-ss", seek, "-i", Narrow(tmp.wstring()),
            "-frames:v", "1", "-vf", std::format("scale='min({0},iw)':'min({0},ih)':force_original_aspect_ratio=decrease", edge),
            "-f", "image2pipe", "-vcodec", "png", "pipe:1" };
        return RunProcess(*m_cfg.ffmpeg, args, { .timeout = 60s, .cancel = st, .logName = "ffmpeg" });
    };
    auto r = grab("1");
    if (r.ok() && r.out.empty())
        r = grab("0"); // clips shorter than a second
    if (r.status == ProcessResult::Status::Cancelled)
        return { ThumbOutcome::Cancelled };
    if (!r.ok() || r.out.empty())
        return { ThumbOutcome::Unsupported, {}, false, "ffmpeg failed" };
    auto img = DecodeImage(Bytes(r.out));
    if (!img)
        return { ThumbOutcome::Unsupported, {}, false, "ffmpeg output undecodable" };
    ThumbResult out;
    out.outcome = ThumbOutcome::Ok;
    out.image = FitWithin(std::move(*img), edge); // ffmpeg applies rotation metadata itself
    return out;
}

uint64_t ThumbDiskUsage(const std::filesystem::path& dir)
{
    uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec))
            total += it->file_size(ec);
    return total;
}

uint64_t TrimThumbDisk(const std::filesystem::path& dir, uint64_t limitBytes)
{
    struct F {
        std::filesystem::path p;
        std::filesystem::file_time_type t;
        uint64_t size;
    };
    std::vector<F> files;
    uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const uint64_t s = it->file_size(ec);
        files.push_back({ it->path(), it->last_write_time(ec), s });
        total += s;
    }
    if (total <= limitBytes)
        return 0;
    std::ranges::sort(files, {}, &F::t); // oldest first
    const uint64_t target = limitBytes / 10 * 9;
    uint64_t freed = 0;
    for (const auto& f : files) {
        if (total - freed <= target)
            break;
        if (std::filesystem::remove(f.p, ec))
            freed += f.size;
    }
    LogInfo("Thumbnail cache trimmed: {} bytes freed", freed);
    return freed;
}

} // namespace ps
