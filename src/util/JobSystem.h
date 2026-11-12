#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ps {

enum class JobPriority { High, Normal, Low };

class Job {
public:
    enum class State { Queued, Running, Done, Failed, Cancelled };

    Job(std::string name, JobPriority prio) : m_name(std::move(name)), m_priority(prio) {}

    const std::string& Name() const { return m_name; }
    JobPriority Priority() const { return m_priority; }
    State GetState() const { return m_state.load(); }
    bool Finished() const { const auto s = GetState(); return s == State::Done || s == State::Failed || s == State::Cancelled; }

    // 0..1, or <0 for indeterminate.
    float Progress() const { return m_progress.load(); }
    void SetProgress(float p) { m_progress.store(p); }

    std::string Detail() const { std::lock_guard l(m_mutex); return m_detail; }
    void SetDetail(std::string d) { std::lock_guard l(m_mutex); m_detail = std::move(d); }
    std::string Error() const { std::lock_guard l(m_mutex); return m_error; }
    void Fail(std::string error) { { std::lock_guard l(m_mutex); m_error = std::move(error); } m_failed = true; }

    void Cancel() { m_stop.request_stop(); }
    std::stop_token Token() const { return m_stop.get_token(); }

private:
    friend class JobSystem;
    std::string m_name;
    JobPriority m_priority;
    std::atomic<State> m_state{ State::Queued };
    std::atomic<float> m_progress{ -1.0f };
    std::atomic<bool> m_failed{ false };
    mutable std::mutex m_mutex;
    std::string m_detail, m_error;
    std::stop_source m_stop;
};

using JobPtr = std::shared_ptr<Job>;
using JobFn = std::function<void(Job&, std::stop_token)>;

// Small worker pool with three priority lanes. Low-priority work (thumbnails)
// may never occupy every worker, so scans and transfers always get a thread.
class JobSystem {
public:
    explicit JobSystem(int workers = 3);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    JobPtr Submit(std::string name, JobPriority prio, JobFn fn);

    // Running + queued jobs at Normal/High priority (for the status bar).
    std::vector<JobPtr> Visible() const;

private:
    struct Item {
        JobPtr job;
        JobFn fn;
    };
    void Worker(std::stop_token st);
    bool PopNext(Item& out);

    mutable std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::deque<Item> m_queues[3];
    std::vector<JobPtr> m_running;
    int m_runningLow = 0;
    int m_workerCount;
    std::vector<std::jthread> m_workers; // last member: joined first
};

} // namespace ps
