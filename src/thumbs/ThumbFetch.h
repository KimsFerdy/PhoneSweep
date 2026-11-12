#pragma once
#include "scan/FileModel.h"
#include "thumbs/ImageOps.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace ps {

class AdbClient;

enum class ThumbVariant : uint8_t { Grid, Preview, Full }; // Full: the file preview window (zoomable)
constexpr int MaxEdge(ThumbVariant v) { return v == ThumbVariant::Grid ? 256 : v == ThumbVariant::Preview ? 640 : 2048; }

struct ThumbRequest {
    std::string serial;
    std::string path; // device path
    uint64_t size = 0;
    int64_t mtime = 0;
    Category category = Category::Other;
    ThumbVariant variant = ThumbVariant::Grid;
    bool tryExif = true; // false for messaging-app media (EXIF stripped) and small files
};

// Stable cache key: device + path + size + mtime + variant (FNV-1a 64).
uint64_t ThumbKey(std::string_view serial, std::string_view path, uint64_t size, int64_t mtime, ThumbVariant v);

enum class ThumbOutcome {
    Ok,
    Unsupported, // format we can't decode (webp, heic, video without ffmpeg, too large): show an icon
    Failed,      // file vanished / corrupt: don't retry this session
    Transient,   // device busy or gone: retry later
    Cancelled,
};

struct ThumbResult {
    ThumbOutcome outcome = ThumbOutcome::Failed;
    Image image;
    bool fromDisk = false;
    std::string note;
};

struct ThumbConfig {
    std::filesystem::path cacheDir;            // %LOCALAPPDATA%/PhoneSweep/thumbs
    std::optional<std::filesystem::path> ffmpeg;
    uint64_t maxImageBytes = 40'000'000;       // skip huge images (panoramas, RAW-ish PNGs)
    uint64_t maxVideoBytes = 400'000'000;      // pulled to a temp file for ffmpeg
};

// Produces one thumbnail: disk cache -> EXIF thumbnail (JPEG grid tiles) ->
// full image -> ffmpeg first frame. Thread-safe; one instance shared by workers.
class ThumbFetcher {
public:
    ThumbFetcher(std::shared_ptr<const AdbClient> adb, ThumbConfig cfg);
    ThumbResult Produce(const ThumbRequest& req, std::stop_token st) const;

    const ThumbConfig& Config() const { return m_cfg; }

private:
    std::filesystem::path CachePath(uint64_t key) const;
    std::optional<Image> FromDisk(uint64_t key) const;
    void ToDisk(uint64_t key, const Image& img) const;
    ThumbResult FromDevice(const ThumbRequest& req, std::stop_token st) const;
    ThumbResult FromVideo(const ThumbRequest& req, uint64_t key, std::stop_token st) const;

    std::shared_ptr<const AdbClient> m_adb;
    ThumbConfig m_cfg;
};

// Disk cache housekeeping (run on a worker).
uint64_t ThumbDiskUsage(const std::filesystem::path& dir);
// Deletes least-recently-used files until usage <= limit * 0.9. Returns bytes freed.
uint64_t TrimThumbDisk(const std::filesystem::path& dir, uint64_t limitBytes);

} // namespace ps
