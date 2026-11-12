#include "util/JobSystem.h"

#include "util/Log.h"

#include <algorithm>
#include <exception>

namespace ps {

JobSystem::JobSystem(int workers)
    : m_workerCount(std::max(2, workers))
{
    for (int i = 0; i < m_workerCount; ++i)
        m_workers.emplace_back([this](std::stop_token st) { Worker(st); });
}

JobSystem::~JobSystem()
{
    {
        std::lock_guard lock(m_mutex);
        for (auto& q : m_queues)
            for (auto& item : q)
                item.job->m_state = Job::State::Cancelled;
        for (auto& j : m_running)
            j->Cancel();
        for (auto& q : m_queues)
            q.clear();
    }
    for (auto& w : m_workers)
        w.request_stop();
    m_cv.notify_all();
    m_workers.clear(); // joins
}

JobPtr JobSystem::Submit(std::string name, JobPriority prio, JobFn fn)
{
    auto job = std::make_shared<Job>(std::move(name), prio);
    {
        std::lock_guard lock(m_mutex);
        m_queues[static_cast<int>(prio)].push_back({ job, std::move(fn) });
    }
    m_cv.notify_one();
    return job;
}

std::vector<JobPtr> JobSystem::Visible() const
{
    std::lock_guard lock(m_mutex);
    std::vector<JobPtr> out;
    for (const auto& j : m_running)
        if (j->Priority() != JobPriority::Low)
            out.push_back(j);
    for (int p = 0; p < 2; ++p)
        for (const auto& item : m_queues[p])
            out.push_back(item.job);
    return out;
}

bool JobSystem::PopNext(Item& out)
{
    for (int p = 0; p < 3; ++p) {
        auto& q = m_queues[p];
        if (q.empty())
            continue;
        if (p == static_cast<int>(JobPriority::Low) && m_runningLow >= m_workerCount - 1)
            return false; // keep one worker free for real work
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
    return false;
}

void JobSystem::Worker(std::stop_token st)
{
    for (;;) {
        Item item;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, st, [&] { return PopNext(item); }))
                return; // stop requested
            m_running.push_back(item.job);
            if (item.job->Priority() == JobPriority::Low)
                ++m_runningLow;
        }

        Job& job = *item.job;
        if (job.Token().stop_requested()) {
            job.m_state = Job::State::Cancelled;
        } else {
            job.m_state = Job::State::Running;
            try {
                item.fn(job, job.Token());
                job.m_state = job.m_failed ? Job::State::Failed
                    : job.Token().stop_requested() ? Job::State::Cancelled
                                                   : Job::State::Done;
            } catch (const std::exception& e) {
                job.Fail(e.what());
                job.m_state = Job::State::Failed;
                LogError("Job '{}' threw: {}", job.Name(), e.what());
            }
        }

        {
            std::lock_guard lock(m_mutex);
            std::erase(m_running, item.job);
            if (item.job->Priority() == JobPriority::Low)
                --m_runningLow;
        }
        m_cv.notify_all(); // a Low slot may have freed up
    }
}

} // namespace ps
