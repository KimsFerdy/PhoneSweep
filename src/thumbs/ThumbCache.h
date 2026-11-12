#pragma once
#include "thumbs/ThumbFetch.h"
#include "util/JobSystem.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps {

class IPlatform;
class AdbClient;
struct FileEntry;

enum class ThumbState : uint8_t { None, Loading, Ready, Unsupported, Failed };

struct ThumbView {
    ThumbState state = ThumbState::None;
    uint64_t texture = 0; // ImTextureID when Ready
    int w = 0, h = 0;
};

// UI-thread front end for thumbnails: lazy requests for visible items, a
// small low-priority worker pool, GPU texture LRU, disk cache via ThumbFetcher.
class ThumbCache {
public:
    ThumbCache(IPlatform& platform, JobSystem& jobs);
    ~ThumbCache();

    // (Re)targets a device. Clears in-memory state; disk cache is kept.
    void SetSource(std::shared_ptr<const AdbClient> adb, std::string serial, ThumbConfig cfg);
    void Reset(); // drop everything (no device)
    void SetEnabled(bool on) { m_enabled = on; }
    bool Enabled() const { return m_enabled; }

    // Call once per frame before drawing: uploads finished thumbnails, evicts, schedules workers.
    void BeginFrame();

    // Ask for a thumbnail of a visible file. Cheap; call every frame for what's on screen.
    ThumbView Request(const FileEntry& file, ThumbVariant variant);
    // Like Request but never queues work (e.g. show the grid thumb while the preview loads).
    ThumbView Peek(const FileEntry& file, ThumbVariant variant) const;

    size_t Pending() const;
    size_t TexturesInUse() const { return m_textures; }
    const ThumbConfig* Config() const { return m_fetcher ? &m_fetcher->Config() : nullptr; }

private:
    struct Entry {
        ThumbState state = ThumbState::None;
        uint64_t texture = 0;
        int w = 0, h = 0;
        uint64_t lastUsedFrame = 0;
        ThumbVariant variant = ThumbVariant::Grid;
        std::chrono::steady_clock::time_point retryAt{};
    };
    struct Job_ {
        uint64_t key;
        ThumbRequest req;
    };
    // Shared with worker jobs; replaced on SetSource so stale workers drain harmlessly.
    struct Shared {
        std::mutex mutex;
        std::deque<Job_> pending; // newest at the back (LIFO: what you just scrolled to wins)
        std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> wanted;
        std::vector<std::pair<uint64_t, ThumbResult>> done;
        std::shared_ptr<const ThumbFetcher> fetcher;
        std::atomic<int> workers{ 0 };
        std::atomic<bool> abandoned{ false };
    };

    uint64_t KeyFor(const FileEntry& f, ThumbVariant v) const;
    void Evict();
    void ReleaseAll();
    static void WorkerLoop(const std::shared_ptr<Shared>& shared, std::stop_token st);

    IPlatform& m_platform;
    JobSystem& m_jobs;
    std::string m_serial;
    std::shared_ptr<const ThumbFetcher> m_fetcher;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::unordered_map<uint64_t, Entry> m_entries;
    uint64_t m_frame = 0;
    size_t m_textures = 0;
    bool m_enabled = true;

    static constexpr int kMaxWorkers = 2;
    static constexpr int kUploadsPerFrame = 6;
    static constexpr size_t kMaxGridTextures = 400;   // ~256x192 RGBA each, ~80 MB worst case
    static constexpr size_t kMaxPreviewTextures = 16; // drawer + duplicate compare columns
    static constexpr size_t kMaxFullTextures = 3;     // file preview window, up to 2048 px (16 MB each)
};

} // namespace ps
