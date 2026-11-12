#pragma once
#include "transfer/LocalPath.h"
#include "transfer/Manifest.h"
#include "util/JobSystem.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps {

class AdbClient;

struct SaveRequest {
    std::string devicePath;
    uint64_t size = 0;    // 0 = unknown (APKs): stat'ed on the phone before pulling
    int64_t mtime = 0;
    std::filesystem::path localRel; // relative to the backup folder
    std::string kind = "file";      // "file" | "apk"
};

struct SaveOptions {
    bool verifyMd5 = true;
    int parallel = 2; // 1..4
    ConflictPolicy conflict = ConflictPolicy::SkipIfIdentical;
};

enum class SaveState : uint8_t {
    Queued,
    Pulling,
    Verifying,
    Done,    // copied + verified (in the manifest)
    Skipped, // identical file was already on the PC (verified, in the manifest)
    Failed,
};
const char* ToString(SaveState s);

struct SaveItem {
    SaveRequest req;
    SaveState state = SaveState::Queued;
    uint64_t bytesDone = 0;
    int attempts = 0;
    std::string error;
    std::filesystem::path finalPath; // absolute, once known
};

// Pull + verify queue for one device and one backup folder (build spec M4).
// Workers run on the JobSystem; the UI thread calls Tick() every frame.
class SaveQueue {
public:
    enum class Status { Idle, Running, Paused, WaitingForDevice, NoSpace, Done };

    struct Stats {
        Status status = Status::Idle;
        size_t total = 0, done = 0, skipped = 0, failed = 0, queued = 0, active = 0;
        uint64_t bytesTotal = 0, bytesDone = 0;
        double bytesPerSec = 0;
        double etaSeconds = -1;
        std::string lastError;
    };

    explicit SaveQueue(JobSystem& jobs);
    ~SaveQueue();
    SaveQueue(const SaveQueue&) = delete;
    SaveQueue& operator=(const SaveQueue&) = delete;

    // Points the queue at a device + folder. Clears it if either changed.
    // serial = adb transport (USB serial or ip:port); deviceId = hardware serial written to the manifest.
    void SetTarget(std::shared_ptr<const AdbClient> adb, std::string serial, std::string deviceId, std::string model, std::filesystem::path folder);
    void SetOptions(SaveOptions opts);
    SaveOptions Options() const;
    std::filesystem::path Folder() const;

    // Returns how many were added (duplicates of queued/finished paths are ignored).
    size_t Enqueue(std::vector<SaveRequest> requests);

    void Pause();
    void Resume();
    void CancelAll();   // drops everything not finished
    void RetryFailed();
    void ClearFinished();
    void SetDeviceConnected(bool connected);

    void Tick(); // UI thread: workers, throughput, space check

    Stats GetStats() const;
    size_t ItemCount() const;
    void Visit(size_t begin, size_t end, const std::function<void(size_t, const SaveItem&)>& fn) const;
    std::optional<SaveState> StateOf(std::string_view devicePath) const;
    // Newly verified entries since the last call (UI adds them to the VerifiedIndex).
    std::vector<ManifestEntry> DrainVerified();

private:
    struct Shared;
    void SpawnWorkers();

    JobSystem& m_jobs;
    std::shared_ptr<Shared> m_s;
};

} // namespace ps
