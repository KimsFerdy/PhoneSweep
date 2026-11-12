#include "transfer/Puller.h"

#include "adb/AdbClient.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <algorithm>
#include <fstream>

namespace ps {

namespace {

constexpr size_t kChunkFiles = 16;             // files per md5sum batch
constexpr uint64_t kChunkBytes = 96ull << 20;  // ...or this many bytes
constexpr int kMaxAttempts = 3;
constexpr uint64_t kSpaceMargin = 200ull << 20;

std::string NowIso()
{
    const auto now = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return std::format("{:%Y-%m-%d %H:%M:%S}", now);
}

bool LooksLikeDeviceGone(const AdbResult& r)
{
    if (r.status == AdbResult::Status::Timeout || r.status == AdbResult::Status::SpawnFailed)
        return true;
    const auto& e = r.err;
    return e.find("no devices") != std::string::npos || e.find("not found") != std::string::npos
        || e.find("offline") != std::string::npos || e.find("device unauthorized") != std::string::npos
        || e.find("closed") != std::string::npos;
}

} // namespace

const char* ToString(SaveState s)
{
    switch (s) {
    case SaveState::Queued: return "queued";
    case SaveState::Pulling: return "pulling";
    case SaveState::Verifying: return "verifying";
    case SaveState::Done: return "saved";
    case SaveState::Skipped: return "already saved";
    case SaveState::Failed: return "failed";
    }
    return "?";
}

struct SaveQueue::Shared {
    mutable std::mutex mutex;
    std::shared_ptr<const AdbClient> adb;
    std::string serial, deviceId, model;
    std::filesystem::path folder;
    std::unique_ptr<Manifest> manifest;
    SaveOptions opts;

    std::vector<SaveItem> items;
    std::unordered_map<std::string, size_t> byPath;
    std::deque<size_t> pending;
    std::vector<ManifestEntry> verified; // drained by the UI

    int workers = 0;
    bool paused = false;
    bool waitingDevice = false;
    bool noSpace = false;
    bool connected = true;
    std::stop_source pauseSrc; // stopped on pause/cancel/disconnect; replaced on resume
    bool cancelling = false;   // CancelAll: in-flight items end as "Cancelled", not re-queued
    std::chrono::steady_clock::time_point waitingSince{};
    std::string lastError;

    // Throughput (UI thread).
    std::atomic<uint64_t> bytesMoved{ 0 }; // monotonically increasing count of streamed bytes
    uint64_t lastBytes = 0;
    std::chrono::steady_clock::time_point lastTick{};
    double rate = 0;

    void StopInFlight()
    {
        pauseSrc.request_stop();
    }
    // Fresh token so new workers can run (after pause/cancel/disconnect).
    void Restart()
    {
        cancelling = false;
        if (pauseSrc.stop_requested())
            pauseSrc = std::stop_source();
    }
    void Requeue(size_t idx, bool front)
    {
        items[idx].bytesDone = 0;
        if (cancelling) {
            items[idx].state = SaveState::Failed;
            items[idx].error = "Cancelled";
            return;
        }
        items[idx].state = SaveState::Queued;
        if (front)
            pending.push_front(idx);
        else
            pending.push_back(idx);
    }
};

SaveQueue::SaveQueue(JobSystem& jobs)
    : m_jobs(jobs)
    , m_s(std::make_shared<Shared>())
{
}

SaveQueue::~SaveQueue()
{
    std::lock_guard l(m_s->mutex);
    m_s->paused = true;
    m_s->StopInFlight();
}

void SaveQueue::SetTarget(std::shared_ptr<const AdbClient> adb, std::string serial, std::string deviceId, std::string model, std::filesystem::path folder)
{
    // Keep the old state alive while its mutex is locked; swap only after unlocking.
    const auto old = m_s;
    SaveOptions opts;
    {
        std::lock_guard l(old->mutex);
        if ((old->serial == serial || (!deviceId.empty() && old->deviceId == deviceId)) && old->folder == folder) {
            old->serial = serial; // same phone, maybe now over Wi-Fi instead of USB
            old->adb = std::move(adb);
            return;
        }
        // Different device/folder: stop and start a fresh queue (workers keep the old Shared).
        old->paused = true;
        old->StopInFlight();
        opts = old->opts;
    }
    auto fresh = std::make_shared<Shared>();
    fresh->opts = opts;
    fresh->adb = std::move(adb);
    fresh->serial = std::move(serial);
    fresh->model = std::move(model);
    fresh->folder = std::move(folder);
    fresh->deviceId = deviceId.empty() ? fresh->serial : std::move(deviceId);
    fresh->manifest = std::make_unique<Manifest>(fresh->folder / L"saved_manifest.json", fresh->deviceId, fresh->model);
    fresh->manifest->Load(); // continue an existing folder's manifest
    m_s = std::move(fresh);
}

void SaveQueue::SetOptions(SaveOptions opts)
{
    opts.parallel = std::clamp(opts.parallel, 1, 4);
    std::lock_guard l(m_s->mutex);
    m_s->opts = opts;
}

SaveOptions SaveQueue::Options() const
{
    std::lock_guard l(m_s->mutex);
    return m_s->opts;
}

std::filesystem::path SaveQueue::Folder() const
{
    std::lock_guard l(m_s->mutex);
    return m_s->folder;
}

size_t SaveQueue::Enqueue(std::vector<SaveRequest> requests)
{
    std::lock_guard l(m_s->mutex);
    size_t added = 0;
    for (auto& r : requests) {
        if (auto it = m_s->byPath.find(r.devicePath); it != m_s->byPath.end()) {
            // Already here: re-queue only if it had failed.
            auto& item = m_s->items[it->second];
            if (item.state == SaveState::Failed) {
                item.attempts = 0;
                item.error.clear();
                m_s->Requeue(it->second, false);
                ++added;
            }
            continue;
        }
        SaveItem item;
        item.req = std::move(r);
        m_s->byPath[item.req.devicePath] = m_s->items.size();
        m_s->pending.push_back(m_s->items.size());
        m_s->items.push_back(std::move(item));
        ++added;
    }
    if (added) {
        m_s->noSpace = false; // re-check in Tick
        if (!m_s->paused && !m_s->waitingDevice)
            m_s->Restart();
    }
    return added;
}

void SaveQueue::Pause()
{
    std::lock_guard l(m_s->mutex);
    m_s->paused = true;
    m_s->StopInFlight(); // in-flight files go back to the queue
}

void SaveQueue::Resume()
{
    std::lock_guard l(m_s->mutex);
    m_s->paused = false;
    m_s->waitingDevice = false;
    m_s->noSpace = false;
    m_s->Restart();
}

void SaveQueue::CancelAll()
{
    // Indices stay stable (workers may still hold them): unfinished items become
    // "Cancelled" failures, which Retry failed can bring back.
    std::lock_guard l(m_s->mutex);
    m_s->cancelling = true;
    m_s->StopInFlight();
    for (size_t idx : m_s->pending) {
        m_s->items[idx].state = SaveState::Failed;
        m_s->items[idx].error = "Cancelled";
    }
    m_s->pending.clear();
    m_s->paused = false;
    m_s->waitingDevice = false;
}

void SaveQueue::RetryFailed()
{
    std::lock_guard l(m_s->mutex);
    if (!m_s->paused && !m_s->waitingDevice)
        m_s->Restart();
    for (size_t i = 0; i < m_s->items.size(); ++i)
        if (m_s->items[i].state == SaveState::Failed) {
            m_s->items[i].attempts = 0;
            m_s->items[i].error.clear();
            m_s->Requeue(i, false);
        }
}

void SaveQueue::ClearFinished()
{
    std::lock_guard l(m_s->mutex);
    if (m_s->workers > 0 || !m_s->pending.empty())
        return; // only when idle, so indices stay stable for workers
    std::vector<SaveItem> kept;
    for (auto& it : m_s->items)
        if (it.state != SaveState::Done && it.state != SaveState::Skipped)
            kept.push_back(std::move(it));
    m_s->items = std::move(kept);
    m_s->byPath.clear();
    for (size_t i = 0; i < m_s->items.size(); ++i)
        m_s->byPath[m_s->items[i].req.devicePath] = i;
}

void SaveQueue::SetDeviceConnected(bool connected)
{
    std::lock_guard l(m_s->mutex);
    if (connected == m_s->connected)
        return;
    m_s->connected = connected;
    if (!connected) {
        m_s->waitingDevice = true;
        m_s->StopInFlight();
        LogWarn("Save queue paused: phone disconnected");
    } else if (m_s->waitingDevice) {
        m_s->waitingDevice = false;
        if (!m_s->paused)
            m_s->Restart();
        LogInfo("Phone reconnected: save queue resumes");
    }
}

void SaveQueue::Tick()
{
    auto s = m_s;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard l(s->mutex);
        // Throughput: exponential moving average over frames.
        const uint64_t moved = s->bytesMoved.load();
        if (s->lastTick != std::chrono::steady_clock::time_point{}) {
            const double dt = std::chrono::duration<double>(now - s->lastTick).count();
            if (dt >= 0.25) {
                const double inst = static_cast<double>(moved - s->lastBytes) / dt;
                s->rate = s->rate == 0 ? inst : s->rate * 0.8 + inst * 0.2;
                s->lastBytes = moved;
                s->lastTick = now;
            }
        } else {
            s->lastTick = now;
            s->lastBytes = moved;
        }
        if (s->workers == 0 && s->pending.empty())
            s->rate = 0;

        // A worker decided the phone was gone (timeout) but the monitor still sees it:
        // give it a few seconds, then try again instead of waiting forever.
        if (s->waitingDevice && s->connected && s->workers == 0) {
            if (s->waitingSince == std::chrono::steady_clock::time_point{})
                s->waitingSince = now;
            else if (now - s->waitingSince > std::chrono::seconds(3)) {
                s->waitingDevice = false;
                s->waitingSince = {};
                if (!s->paused)
                    s->Restart();
                LogInfo("Phone still connected: save queue retries");
            }
        } else if (!s->waitingDevice) {
            s->waitingSince = {};
        }

        // Space check before starting more work.
        if (!s->pending.empty() && !s->noSpace && !s->folder.empty()) {
            uint64_t remaining = 0;
            for (size_t idx : s->pending)
                remaining += s->items[idx].req.size;
            const uint64_t free = FreeSpace(s->folder);
            if (free > 0 && free < remaining + kSpaceMargin) {
                s->noSpace = true;
                s->lastError = std::format("Not enough space on the PC: {} needed, {} free", FormatBytes(remaining + kSpaceMargin), FormatBytes(free));
                LogError("{}", s->lastError);
            }
        }
    }
    SpawnWorkers();
}

void SaveQueue::SpawnWorkers()
{
    auto s = m_s;
    int toSpawn = 0;
    {
        std::lock_guard l(s->mutex);
        if (s->paused || s->waitingDevice || s->noSpace || !s->connected || !s->adb || s->pending.empty())
            return;
        toSpawn = std::max(0, std::min<int>(s->opts.parallel, static_cast<int>(s->pending.size())) - s->workers);
        s->workers += toSpawn;
    }
    for (int i = 0; i < toSpawn; ++i)
        m_jobs.Submit("Saving to PC", JobPriority::Normal, [s](Job&, std::stop_token st) {
            // Run against the Shared captured at spawn time; if the target changes, this
            // worker drains harmlessly because its pauseSrc has been stopped.
            auto keep = s;
            struct Guard {
                std::shared_ptr<Shared> s;
                ~Guard()
                {
                    std::lock_guard l(s->mutex);
                    --s->workers;
                }
            } guard{ keep };
            // Translate app shutdown (job token) into a pause of in-flight transfers.
            std::stop_callback onJobStop(st, [keep] { keep->pauseSrc.request_stop(); });
            // Worker body (inlined to bind to `keep`, not m_s).
            auto& S = *keep;
            for (;;) {
                std::vector<size_t> chunk;
                std::stop_token tok;
                std::shared_ptr<const AdbClient> adb;
                std::string serial;
                std::filesystem::path folder;
                SaveOptions opts;
                {
                    std::lock_guard l(S.mutex);
                    if (S.paused || S.waitingDevice || S.noSpace || st.stop_requested() || S.pauseSrc.stop_requested())
                        return;
                    uint64_t bytes = 0;
                    // Fair share so parallel workers all get files (but at least 1, at most kChunkFiles).
                    const size_t share = std::clamp<size_t>((S.pending.size() + S.opts.parallel - 1) / std::max(1, S.opts.parallel), 1, kChunkFiles);
                    while (!S.pending.empty() && chunk.size() < share && (chunk.empty() || bytes < kChunkBytes)) {
                        const size_t idx = S.pending.front();
                        S.pending.pop_front();
                        if (S.items[idx].state != SaveState::Queued)
                            continue;
                        S.items[idx].state = SaveState::Pulling;
                        bytes += S.items[idx].req.size;
                        chunk.push_back(idx);
                    }
                    if (chunk.empty())
                        return;
                    tok = S.pauseSrc.get_token();
                    adb = S.adb;
                    serial = S.serial;
                    folder = S.folder;
                    opts = S.opts;
                }

                // ---- Pull each file (streamed: write + MD5 on the fly) ----
                struct Pulled {
                    size_t idx;
                    std::filesystem::path part, target;
                    std::string localMd5;
                    bool precheck = false; // identical file already on PC; nothing was pulled
                };
                std::vector<Pulled> pulled;
                bool deviceGone = false;

                for (size_t idx : chunk) {
                    SaveRequest req;
                    {
                        std::lock_guard l(S.mutex);
                        req = S.items[idx].req;
                    }
                    if (tok.stop_requested() || deviceGone) {
                        std::lock_guard l(S.mutex);
                        S.Requeue(idx, true);
                        continue;
                    }
                    // Unknown size (APKs): ask the phone.
                    if (req.size == 0) {
                        const auto r = adb->Shell(serial, ShellCommand("stat").arg("-c").arg("%s %Y").arg(req.devicePath), { .timeout = 20s, .cancel = tok });
                        const auto t = SplitWhitespace(r.out);
                        if (r.ok() && t.size() >= 2) {
                            req.size = ParseInt<uint64_t>(t[0]).value_or(0);
                            req.mtime = ParseInt<int64_t>(t[1]).value_or(0);
                            std::lock_guard l(S.mutex);
                            S.items[idx].req.size = req.size;
                            S.items[idx].req.mtime = req.mtime;
                        }
                    }
                    const auto target = folder / req.localRel;
                    std::error_code ec;
                    std::filesystem::create_directories(LongPath(target.parent_path()), ec);

                    // Identical file already there? Verify it instead of pulling again.
                    if (opts.conflict == ConflictPolicy::SkipIfIdentical && std::filesystem::exists(LongPath(target), ec)
                        && std::filesystem::file_size(LongPath(target), ec) == req.size && req.size > 0) {
                        Pulled p{ idx, {}, target, {}, true };
                        if (opts.verifyMd5) {
                            if (auto h = Md5File(LongPath(target)))
                                p.localMd5 = *h;
                        }
                        {
                            std::lock_guard l(S.mutex);
                            S.items[idx].state = SaveState::Verifying;
                        }
                        pulled.push_back(std::move(p));
                        continue;
                    }

                    auto part = target;
                    part += L".part";
                    std::ofstream out(LongPath(part), std::ios::binary | std::ios::trunc);
                    if (!out) {
                        std::lock_guard l(S.mutex);
                        S.items[idx].state = SaveState::Failed;
                        S.items[idx].error = "Couldn't create the file on the PC (permissions or invalid path)";
                        continue;
                    }
                    Md5 md5;
                    uint64_t written = 0, sinceUpdate = 0;
                    bool writeFailed = false;
                    // Stream `exec-out cat` straight to disk (binary-safe; never buffered whole).
                    const std::string args[] = { "-s", serial, "exec-out", ShellCommand("cat").arg(req.devicePath).str() };
                    const auto rs = RunProcess(adb->ExePath(), args, { .timeout = std::chrono::minutes(30), .cancel = tok, .logName = "adb" },
                        [&](std::string_view data) {
                            if (writeFailed)
                                return;
                            out.write(data.data(), static_cast<std::streamsize>(data.size()));
                            if (!out) {
                                writeFailed = true;
                                return;
                            }
                            md5.Update(data);
                            written += data.size();
                            sinceUpdate += data.size();
                            S.bytesMoved += data.size();
                            if (sinceUpdate >= (1u << 20)) {
                                sinceUpdate = 0;
                                std::lock_guard l(S.mutex);
                                S.items[idx].bytesDone = written;
                            }
                        });
                    out.close();

                    std::lock_guard l(S.mutex);
                    S.items[idx].bytesDone = written;
                    if (rs.status == ProcessResult::Status::Cancelled) {
                        std::filesystem::remove(LongPath(part), ec);
                        S.Requeue(idx, true);
                        continue;
                    }
                    if (writeFailed) {
                        std::filesystem::remove(LongPath(part), ec);
                        S.items[idx].state = SaveState::Failed;
                        S.items[idx].error = "Writing to the PC failed (disk full?)";
                        S.noSpace = true;
                        S.lastError = S.items[idx].error;
                        continue;
                    }
                    if (!rs.ok() || written != req.size) {
                        std::filesystem::remove(LongPath(part), ec);
                        if (LooksLikeDeviceGone(rs) || !S.connected) {
                            deviceGone = true;
                            S.waitingDevice = true;
                            S.StopInFlight();
                            S.Requeue(idx, true);
                            continue;
                        }
                        auto& it = S.items[idx];
                        ++it.attempts;
                        it.error = !rs.ok() ? (rs.err.empty() ? std::string("Copy failed") : rs.err)
                                            : std::format("Size mismatch: expected {} bytes, got {} (file changed on the phone?)", req.size, written);
                        if (it.attempts < kMaxAttempts)
                            S.Requeue(idx, false);
                        else
                            it.state = SaveState::Failed;
                        continue;
                    }
                    S.items[idx].state = opts.verifyMd5 ? SaveState::Verifying : SaveState::Pulling;
                    pulled.push_back({ idx, part, target, md5.HexFinal(), false });
                }

                // ---- Verify: one phone-side md5 call for the whole chunk ----
                std::vector<std::string> deviceMd5(pulled.size());
                bool md5Ok = true;
                if (opts.verifyMd5 && !pulled.empty() && !deviceGone) {
                    // One output line per file, in order, even for odd names: hash or "-".
                    ShellCommand cmd("for f in");
                    {
                        std::lock_guard l(S.mutex);
                        for (const auto& p : pulled)
                            cmd.arg(S.items[p.idx].req.devicePath);
                    }
                    cmd.raw("; do h=$(md5sum \"$f\" 2>/dev/null) && echo \"${h%% *}\" || echo -; done");
                    const auto r = adb->Shell(serial, cmd, { .timeout = std::chrono::minutes(10), .cancel = tok });
                    std::vector<std::string> lines;
                    ForEachLine(r.out, [&](std::string_view line) {
                        line = Trim(line);
                        if (!line.empty())
                            lines.emplace_back(line);
                    });
                    if (!r.ok() || lines.size() != pulled.size()) {
                        md5Ok = false;
                        if (LooksLikeDeviceGone(r) || r.status == ProcessResult::Status::Cancelled)
                            deviceGone = true;
                    } else {
                        deviceMd5 = std::move(lines);
                    }
                }

                // ---- Finalize ----
                bool manifestDirty = false;
                for (size_t k = 0; k < pulled.size(); ++k) {
                    auto& p = pulled[k];
                    std::error_code ec;
                    std::lock_guard l(S.mutex);
                    auto& item = S.items[p.idx];
                    if (deviceGone || (opts.verifyMd5 && !md5Ok)) {
                        if (!p.part.empty())
                            std::filesystem::remove(LongPath(p.part), ec);
                        if (deviceGone) {
                            S.waitingDevice = true;
                            S.Requeue(p.idx, true);
                        } else {
                            ++item.attempts;
                            item.error = "Couldn't read checksums from the phone";
                            if (item.attempts < kMaxAttempts)
                                S.Requeue(p.idx, false);
                            else
                                item.state = SaveState::Failed;
                        }
                        continue;
                    }
                    const bool match = !opts.verifyMd5 || (deviceMd5[k] != "-" && deviceMd5[k] == p.localMd5);
                    if (p.precheck) {
                        if (match) {
                            item.state = SaveState::Skipped;
                            item.finalPath = p.target;
                        } else {
                            // Same size, different content: pull it next to the old one.
                            item.req.localRel = std::filesystem::relative(NextFreeName(p.target), folder, ec);
                            S.Requeue(p.idx, false);
                            continue;
                        }
                    } else {
                        if (!match) {
                            std::filesystem::remove(LongPath(p.part), ec);
                            ++item.attempts;
                            item.error = deviceMd5[k] == "-" ? "The phone couldn't checksum this file" : "Checksum mismatch (MD5 differs)";
                            if (item.attempts < kMaxAttempts)
                                S.Requeue(p.idx, false);
                            else
                                item.state = SaveState::Failed;
                            continue;
                        }
                        // Place the verified file under its final name.
                        std::filesystem::path final = p.target;
                        if (std::filesystem::exists(LongPath(final), ec) && S.opts.conflict != ConflictPolicy::Overwrite)
                            final = NextFreeName(final);
                        std::filesystem::rename(LongPath(p.part), LongPath(final), ec);
                        if (ec) {
                            std::filesystem::remove(LongPath(p.part), ec);
                            item.state = SaveState::Failed;
                            item.error = "Couldn't move the file into place: " + ec.message();
                            continue;
                        }
                        SetFileMtime(final, item.req.mtime);
                        item.finalPath = final;
                        item.state = SaveState::Done;
                    }
                    item.bytesDone = item.req.size;
                    ManifestEntry e;
                    e.devicePath = item.req.devicePath;
                    e.size = item.req.size;
                    e.mtime = item.req.mtime;
                    e.md5 = opts.verifyMd5 ? p.localMd5 : std::string();
                    e.localRel = Narrow(std::filesystem::relative(item.finalPath, folder, ec).generic_wstring());
                    e.verifiedAt = NowIso();
                    e.kind = item.req.kind;
                    S.manifest->Upsert(e);
                    S.verified.push_back(std::move(e));
                    manifestDirty = true;
                }
                if (manifestDirty && !S.manifest->Save())
                    LogError("Couldn't write {}", Narrow(S.manifest->File().wstring()));
                if (deviceGone) {
                    LogWarn("Phone not responding: save queue waits for it");
                    return;
                }
            }
        });
}

SaveQueue::Stats SaveQueue::GetStats() const
{
    std::lock_guard l(m_s->mutex);
    Stats st;
    st.total = m_s->items.size();
    for (const auto& it : m_s->items) {
        switch (it.state) {
        case SaveState::Done: ++st.done; st.bytesDone += it.req.size; break;
        case SaveState::Skipped: ++st.skipped; st.bytesDone += it.req.size; break;
        case SaveState::Failed: ++st.failed; break;
        case SaveState::Queued: ++st.queued; break;
        default: ++st.active; st.bytesDone += it.bytesDone; break;
        }
        if (it.state != SaveState::Failed)
            st.bytesTotal += it.req.size;
    }
    st.bytesPerSec = m_s->rate;
    if (st.bytesPerSec > 1000 && st.bytesTotal > st.bytesDone)
        st.etaSeconds = static_cast<double>(st.bytesTotal - st.bytesDone) / st.bytesPerSec;
    st.lastError = m_s->lastError;
    if (m_s->noSpace)
        st.status = Status::NoSpace;
    else if (m_s->waitingDevice || !m_s->connected)
        st.status = (st.queued + st.active) ? Status::WaitingForDevice : Status::Idle;
    else if (m_s->paused)
        st.status = Status::Paused;
    else if (st.queued + st.active > 0)
        st.status = Status::Running;
    else if (st.total > 0)
        st.status = Status::Done;
    return st;
}

size_t SaveQueue::ItemCount() const
{
    std::lock_guard l(m_s->mutex);
    return m_s->items.size();
}

void SaveQueue::Visit(size_t begin, size_t end, const std::function<void(size_t, const SaveItem&)>& fn) const
{
    std::lock_guard l(m_s->mutex);
    end = std::min(end, m_s->items.size());
    for (size_t i = begin; i < end; ++i)
        fn(i, m_s->items[i]);
}

std::optional<SaveState> SaveQueue::StateOf(std::string_view devicePath) const
{
    std::lock_guard l(m_s->mutex);
    auto it = m_s->byPath.find(std::string(devicePath));
    if (it == m_s->byPath.end())
        return std::nullopt;
    return m_s->items[it->second].state;
}

std::vector<ManifestEntry> SaveQueue::DrainVerified()
{
    std::lock_guard l(m_s->mutex);
    std::vector<ManifestEntry> out;
    out.swap(m_s->verified);
    return out;
}

} // namespace ps
