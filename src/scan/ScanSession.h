#pragma once
#include "scan/FileScanner.h"
#include "scan/FileStore.h"
#include "util/JobSystem.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

// One file scan of one device: runs on the JobSystem, hands results to the UI
// thread through Pump(). Restartable.
class ScanSession {
public:
    enum class State { Idle, Running, Done, Failed, Cancelled };

    void Start(JobSystem& jobs, std::shared_ptr<const AdbClient> adb, std::string serial, ScanOptions opts);
    void Cancel();

    // UI thread: move finished batches into the store. Returns true if anything changed.
    bool Pump(FileStore& store);

    State GetState() const;
    bool Running() const { return GetState() == State::Running; }
    std::string Phase() const;
    size_t FilesFound() const { return m_shared->filesFound.load(); }
    ScanOutcome Outcome() const;
    std::chrono::steady_clock::duration Elapsed() const;

private:
    struct Shared {
        std::mutex mutex;
        std::vector<std::vector<FileEntry>> batches;
        std::vector<Volume> volumes;
        bool volumesReady = false;
        std::vector<MediaRow> mime;
        bool mimeReady = false;
        bool clearPending = false;
        std::string phase;
        ScanOutcome outcome;
        State state = State::Idle;
        std::chrono::steady_clock::time_point start, end;
        std::atomic<size_t> filesFound{ 0 };
    };

    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    JobPtr m_job;
};

} // namespace ps
