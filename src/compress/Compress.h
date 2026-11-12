#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class AdbClient;
class FileStore;
class VerifiedIndex;

// ---- Planning (pure, unit-tested) ---------------------------------------------

struct MediaInfo {
    int64_t durationMs = 0; // videos only
    int width = 0, height = 0;
};
// `content query ... --projection _data:duration:width:height:_size` (duration optional).
std::map<std::string, MediaInfo, std::less<>> ParseMediaInfoRows(std::string_view text);

enum class MediaKind : uint8_t { Video, Photo };

struct CompressCandidate {
    uint32_t file = 0; // FileStore index at planning time
    std::string path;
    MediaKind kind = MediaKind::Video;
    uint64_t size = 0;
    int64_t mtime = 0;
    MediaInfo info;
    uint64_t estimatedSize = 0;
    bool saved = false; // original verified on the PC
    uint64_t EstimatedSaving() const { return size > estimatedSize ? size - estimatedSize : 0; }
};

// "Balanced" preset (user choice): H.264 CRF 26, long edge <= 1920, AAC 128k; JPEG quality 85.
int TargetVideoKbps(int width, int height);             // video stream, after downscaling
std::optional<uint64_t> EstimateVideo(uint64_t size, const MediaInfo& info); // nullopt = not worth it
std::optional<uint64_t> EstimatePhoto(uint64_t size, const MediaInfo& info);

// Files that are expected to shrink by >= 20%, biggest saving first.
std::vector<CompressCandidate> FindCompressCandidates(const FileStore& files, const VerifiedIndex& verified,
    const std::map<std::string, MediaInfo, std::less<>>& media);

// ffmpeg arguments for one video. qsv = Intel Quick Sync (much faster), else libx264.
std::vector<std::string> VideoArgs(const std::filesystem::path& in, const std::filesystem::path& out, const MediaInfo& info, bool qsv);

// Re-encode a JPEG at `quality`, keeping the original EXIF block (date, GPS, orientation).
std::optional<std::string> CompressJpeg(std::string_view original, int quality = 85);
// Inserts the APP1/Exif segment of `original` right after SOI of `encoded` (exposed for tests).
std::string SpliceExif(std::string_view original, std::string_view encoded);

// Name on the phone after compression (videos become .mp4).
std::string CompressedPhonePath(std::string_view original, MediaKind kind);

// ---- Running ---------------------------------------------------------------------

enum class CompressOutcome : uint8_t {
    PhoneAndPc, // compressed replaces the original on the phone, copy saved on the PC
    PhoneOnly,  // compressed replaces the original on the phone
    PcOnly,     // compressed saved on the PC, original deleted from the phone
};

struct CompressItemResult {
    std::string path;    // original
    std::string newPath; // on the phone (empty if removed / skipped)
    uint64_t before = 0, after = 0;
    std::string status;  // "compressed", "already efficient", error text
    bool ok = false;
};

struct CompressProgress {
    std::mutex mutex;
    size_t index = 0, total = 0;
    std::string current, step;
    float fileFraction = 0; // 0..1 within the current file (ffmpeg progress)
    bool done = false;
    std::vector<CompressItemResult> results;
};

struct CompressTask {
    std::string serial;
    std::vector<CompressCandidate> items;
    CompressOutcome outcome = CompressOutcome::PhoneAndPc;
    std::filesystem::path ffmpeg;
    bool useQsv = true;
    std::filesystem::path pcFolder;  // <backup folder>/compressed
    std::vector<std::string> volumeRoots, volumeLabels;
    std::filesystem::path tmpDir;
};

void RunCompress(const AdbClient& adb, const CompressTask& task, CompressProgress& progress, std::stop_token st);

// Does this ffmpeg have a working h264_qsv encoder? (Encodes one black frame.)
bool ProbeQsv(const std::filesystem::path& ffmpeg, const std::filesystem::path& tmpDir);

} // namespace ps
