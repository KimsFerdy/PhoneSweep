#include "thumbs/ThumbCache.h"

#include "platform/Platform.h"
#include "scan/FileModel.h"
#include "util/Log.h"

#include <algorithm>

namespace ps {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kStaleAfter = std::chrono::milliseconds(750); // scrolled out of view: skip it
constexpr auto kRetryAfter = std::chrono::seconds(8);        // transient adb failure

bool WantsThumb(const FileEntry& f) { return f.category == Category::Images || f.category == Category::Videos; }

} // namespace

ThumbCache::ThumbCache(IPlatform& platform, JobSystem& jobs)
    : m_platform(platform)
    , m_jobs(jobs)
{
}

ThumbCache::~ThumbCache()
{
    m_shared->abandoned = true;
    ReleaseAll();
}

void ThumbCache::ReleaseAll()
{
    for (auto& [key, e] : m_entries)
        if (e.texture)
            m_platform.DestroyTexture(e.texture);
    m_entries.clear();
    m_textures = 0;
}

void ThumbCache::Reset()
{
    m_shared->abandoned = true;
    m_shared = std::make_shared<Shared>();
    m_fetcher.reset();
    m_serial.clear();
    ReleaseAll();
}

void ThumbCache::SetSource(std::shared_ptr<const AdbClient> adb, std::string serial, ThumbConfig cfg)
{
    Reset();
    m_serial = std::move(serial);
    m_fetcher = std::make_shared<ThumbFetcher>(std::move(adb), std::move(cfg));
    m_shared->fetcher = m_fetcher;
}

uint64_t ThumbCache::KeyFor(const FileEntry& f, ThumbVariant v) const { return ThumbKey(m_serial, f.path, f.size, f.mtime, v); }

size_t ThumbCache::Pending() const
{
    std::lock_guard l(m_shared->mutex);
    return m_shared->pending.size();
}

ThumbView ThumbCache::Peek(const FileEntry& f, ThumbVariant v) const
{
    if (!m_fetcher || !WantsThumb(f))
        return {};
    auto it = m_entries.find(KeyFor(f, v));
    if (it == m_entries.end())
        return {};
    return { it->second.state, it->second.texture, it->second.w, it->second.h };
}

ThumbView ThumbCache::Request(const FileEntry& f, ThumbVariant v)
{
    if (!m_enabled || !m_fetcher || !WantsThumb(f))
        return {};
    const uint64_t key = KeyFor(f, v);
    auto [it, inserted] = m_entries.try_emplace(key);
    Entry& e = it->second;
    e.lastUsedFrame = m_frame;
    e.variant = v;

    const auto now = Clock::now();
    const bool retry = e.state == ThumbState::None && e.retryAt != Clock::time_point{} && now >= e.retryAt;
    if (inserted || retry) {
        e.state = ThumbState::Loading;
        e.retryAt = {};
        ThumbRequest req{ m_serial, f.path, f.size, f.mtime, f.category, v };
        // WhatsApp/Telegram strip EXIF, and small files are as quick to pull whole.
        const bool messaging = f.source == Source::WhatsApp || f.source == Source::WhatsAppBusiness || f.source == Source::Telegram;
        req.tryExif = !messaging && f.size > 600'000;
        std::lock_guard l(m_shared->mutex);
        m_shared->pending.push_back({ key, std::move(req) });
        m_shared->wanted[key] = now;
    } else if (e.state == ThumbState::Loading) {
        std::lock_guard l(m_shared->mutex);
        m_shared->wanted[key] = now; // still on screen: keep it fresh
    } else if (e.state == ThumbState::None) {
        return { ThumbState::Loading }; // waiting out a transient failure
    }
    return { e.state, e.texture, e.w, e.h };
}

void ThumbCache::BeginFrame()
{
    ++m_frame;

    // 1. Collect finished work (bounded GPU uploads per frame; the rest waits).
    std::vector<std::pair<uint64_t, ThumbResult>> done;
    {
        std::lock_guard l(m_shared->mutex);
        const size_t take = std::min<size_t>(m_shared->done.size(), kUploadsPerFrame);
        done.assign(std::make_move_iterator(m_shared->done.begin()), std::make_move_iterator(m_shared->done.begin() + take));
        m_shared->done.erase(m_shared->done.begin(), m_shared->done.begin() + take);
    }
    for (auto& [key, res] : done) {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            continue; // evicted meanwhile
        Entry& e = it->second;
        switch (res.outcome) {
        case ThumbOutcome::Ok:
            if (e.texture)
                break;
            e.texture = m_platform.CreateTexture(res.image.rgba.data(), res.image.w, res.image.h);
            e.state = e.texture ? ThumbState::Ready : ThumbState::Failed;
            e.w = res.image.w;
            e.h = res.image.h;
            m_textures += e.texture != 0;
            break;
        case ThumbOutcome::Unsupported: e.state = ThumbState::Unsupported; break;
        case ThumbOutcome::Failed: e.state = ThumbState::Failed; break;
        case ThumbOutcome::Transient:
            e.state = ThumbState::None;
            e.retryAt = Clock::now() + kRetryAfter;
            break;
        case ThumbOutcome::Cancelled:
            m_entries.erase(it); // dropped because it scrolled away; re-request when visible
            break;
        }
    }

    Evict();

    // 2. Keep up to kMaxWorkers low-priority fetchers running while there is work.
    if (!m_fetcher)
        return;
    size_t pending;
    {
        std::lock_guard l(m_shared->mutex);
        pending = m_shared->pending.size();
    }
    while (pending > 0 && m_shared->workers.load() < kMaxWorkers) {
        ++m_shared->workers;
        auto shared = m_shared;
        m_jobs.Submit("Thumbnails", JobPriority::Low, [shared](Job&, std::stop_token st) { WorkerLoop(shared, st); });
        --pending;
    }
}

void ThumbCache::WorkerLoop(const std::shared_ptr<Shared>& shared, std::stop_token st)
{
    struct Done {
        Shared& s;
        ~Done() { --s.workers; }
    } guard{ *shared };

    while (!st.stop_requested() && !shared->abandoned) {
        Job_ job;
        {
            std::lock_guard l(shared->mutex);
            if (shared->pending.empty())
                return;
            job = std::move(shared->pending.back());
            shared->pending.pop_back();
            // Not asked for recently = scrolled off screen. Hand it back as cancelled.
            auto w = shared->wanted.find(job.key);
            if (w == shared->wanted.end() || Clock::now() - w->second > kStaleAfter) {
                shared->wanted.erase(job.key);
                shared->done.push_back({ job.key, ThumbResult{ ThumbOutcome::Cancelled } });
                continue;
            }
            shared->wanted.erase(w);
        }
        ThumbResult res = shared->fetcher->Produce(job.req, st);
        if (res.outcome == ThumbOutcome::Ok && !res.fromDisk && !res.note.empty())
            LogInfo("Thumbnail via {}: {}", res.note, job.req.path);
        std::lock_guard l(shared->mutex);
        shared->done.push_back({ job.key, std::move(res) });
    }
}

void ThumbCache::Evict()
{
    // Count textures per variant; drop least-recently-used ones not drawn last frame.
    size_t counts[3] = { 0, 0, 0 }; // per ThumbVariant
    static constexpr size_t kCaps[3] = { kMaxGridTextures, kMaxPreviewTextures, kMaxFullTextures };
    for (const auto& [k, e] : m_entries)
        if (e.texture)
            counts[static_cast<size_t>(e.variant)]++;
    if (counts[0] <= kCaps[0] && counts[1] <= kCaps[1] && counts[2] <= kCaps[2])
        return;

    std::vector<std::pair<uint64_t, uint64_t>> lru; // (lastUsedFrame, key)
    for (const auto& [k, e] : m_entries)
        if (e.texture && e.lastUsedFrame + 1 < m_frame)
            lru.push_back({ e.lastUsedFrame, k });
    std::ranges::sort(lru);
    for (const auto& [frame, key] : lru) {
        auto it = m_entries.find(key);
        auto& count = counts[static_cast<size_t>(it->second.variant)];
        const size_t cap = kCaps[static_cast<size_t>(it->second.variant)];
        if (count <= cap)
            continue;
        m_platform.DestroyTexture(it->second.texture);
        --m_textures;
        --count;
        m_entries.erase(it); // disk cache makes reloading cheap
    }
}

} // namespace ps
