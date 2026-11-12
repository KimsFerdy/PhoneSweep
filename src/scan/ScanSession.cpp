#include "scan/ScanSession.h"

#include "adb/AdbClient.h"
#include "util/Log.h"

#include <format>

namespace ps {

void ScanSession::Start(JobSystem& jobs, std::shared_ptr<const AdbClient> adb, std::string serial, ScanOptions opts)
{
    Cancel();
    // A fresh Shared block: a cancelled job still finishing writes into the old one.
    m_shared = std::make_shared<Shared>();
    {
        std::lock_guard l(m_shared->mutex);
        m_shared->state = State::Running;
        m_shared->clearPending = true;
        m_shared->start = std::chrono::steady_clock::now();
        m_shared->phase = "Starting";
    }

    auto shared = m_shared;
    m_job = jobs.Submit("Scanning files", JobPriority::Normal, [shared, adb, serial, opts](Job& job, std::stop_token st) {
        auto* counter = &shared->filesFound;
        ScanSink sink;
        sink.onVolumes = [&](const std::vector<Volume>& v) {
            std::lock_guard l(shared->mutex);
            shared->volumes = v;
            shared->volumesReady = true;
        };
        sink.onBatch = [&](std::vector<FileEntry>&& b) {
            *counter += b.size();
            job.SetDetail(std::format("{} files", counter->load()));
            std::lock_guard l(shared->mutex);
            shared->batches.push_back(std::move(b));
        };
        sink.onPhase = [&](std::string p) {
            job.SetDetail(p);
            std::lock_guard l(shared->mutex);
            shared->phase = std::move(p);
        };
        sink.onMediaStore = [&](std::vector<MediaRow>&& rows) {
            std::lock_guard l(shared->mutex);
            shared->mime = std::move(rows);
            shared->mimeReady = true;
        };

        LogInfo("Scan started");
        ScanOutcome outcome = FileScanner::Run(*adb, serial, opts, sink, st);

        std::lock_guard l(shared->mutex);
        shared->end = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(shared->end - shared->start).count();
        if (outcome.cancelled) {
            shared->state = State::Cancelled;
            LogInfo("Scan cancelled after {} files", outcome.files);
        } else if (!outcome.ok) {
            shared->state = State::Failed;
            job.Fail(outcome.error);
            LogError("Scan failed: {} ({})", outcome.error, outcome.errorDetail);
        } else {
            shared->state = State::Done;
            LogInfo("Scan finished: {} files in {:.1f} s via {}", outcome.files, ms / 1000.0, ToString(outcome.method));
        }
        shared->outcome = std::move(outcome);
    });
}

void ScanSession::Cancel()
{
    if (m_job && !m_job->Finished())
        m_job->Cancel();
}

bool ScanSession::Pump(FileStore& store)
{
    std::vector<std::vector<FileEntry>> batches;
    std::vector<Volume> volumes;
    std::vector<MediaRow> mime;
    bool clear = false, haveVolumes = false, haveMime = false;
    {
        std::lock_guard l(m_shared->mutex);
        clear = std::exchange(m_shared->clearPending, false);
        if (m_shared->volumesReady) {
            volumes = std::move(m_shared->volumes);
            m_shared->volumesReady = false;
            haveVolumes = true;
        }
        batches.swap(m_shared->batches);
        if (m_shared->mimeReady) {
            mime = std::move(m_shared->mime);
            m_shared->mimeReady = false;
            haveMime = true;
        }
    }
    if (clear)
        store.Clear();
    if (haveVolumes)
        store.SetVolumes(std::move(volumes));
    for (auto& b : batches)
        store.Append(std::move(b));
    if (haveMime) {
        const size_t changed = store.ApplyMime(mime);
        LogInfo("MediaStore re-categorized {} files", changed);
    }
    return clear || haveVolumes || !batches.empty() || haveMime;
}

ScanSession::State ScanSession::GetState() const
{
    std::lock_guard l(m_shared->mutex);
    return m_shared->state;
}

std::string ScanSession::Phase() const
{
    std::lock_guard l(m_shared->mutex);
    return m_shared->phase;
}

ScanOutcome ScanSession::Outcome() const
{
    std::lock_guard l(m_shared->mutex);
    return m_shared->outcome;
}

std::chrono::steady_clock::duration ScanSession::Elapsed() const
{
    std::lock_guard l(m_shared->mutex);
    const auto end = m_shared->state == State::Running ? std::chrono::steady_clock::now() : m_shared->end;
    return end - m_shared->start;
}

} // namespace ps
