#include "scan/AppSession.h"

#include "adb/AdbClient.h"
#include "app/Settings.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <algorithm>

namespace ps {

AppSession::AppSession()
    : m_labelCache(std::make_shared<LabelCache>(Settings::DataDir() / L"labels.json"))
    , m_aapt2(FindAapt2())
{
    if (m_aapt2)
        LogInfo("aapt2 found: {} (app labels available)", Narrow(m_aapt2->wstring()));
    else
        LogInfo("aapt2 not found: apps are shown by package name");
}

AppSession::~AppSession() { m_labels->abandoned = true; }

void AppSession::Clear()
{
    Cancel();
    m_labels->abandoned = true;
    m_labels = std::make_shared<LabelShared>();
    m_scan = std::make_shared<ScanShared>();
    m_apps.clear();
    m_uninstall.clear();
    m_labelRequested.clear();
    ++m_version;
}

void AppSession::Start(JobSystem& jobs, std::shared_ptr<const AdbClient> adb, std::string serial, int deviceSdk)
{
    Cancel();
    if (serial != m_serial) {
        Clear();
        m_serial = serial;
    }
    m_adb = adb;
    m_scan = std::make_shared<ScanShared>();
    m_scan->state = State::Running;

    auto shared = m_scan;
    m_job = jobs.Submit("Scanning apps", JobPriority::Normal, [shared, adb, serial, deviceSdk](Job& job, std::stop_token st) {
        job.SetDetail("reading packages");
        AppScanResult r = ScanApps(*adb, serial, deviceSdk, st);
        std::lock_guard l(shared->mutex);
        shared->state = r.cancelled ? State::Cancelled : r.ok ? State::Done : State::Failed;
        if (!r.ok && !r.cancelled) {
            job.Fail(r.error);
            LogError("App scan failed: {} ({})", r.error, r.errorDetail);
        }
        shared->result = std::move(r);
        shared->resultReady = true;
    });
}

void AppSession::Cancel()
{
    if (m_job && !m_job->Finished())
        m_job->Cancel();
}

AppSession::State AppSession::GetState() const
{
    std::lock_guard l(m_scan->mutex);
    return m_scan->state;
}

AppScanResult AppSession::Outcome() const
{
    std::lock_guard l(m_scan->mutex);
    AppScanResult r;
    r.ok = m_scan->result.ok;
    r.cancelled = m_scan->result.cancelled;
    r.error = m_scan->result.error;
    r.errorDetail = m_scan->result.errorDetail;
    r.launcherKnown = m_scan->result.launcherKnown;
    return r;
}

bool AppSession::Pump(JobSystem& jobs)
{
    bool changed = false;
    {
        std::lock_guard l(m_scan->mutex);
        if (m_scan->resultReady) {
            m_scan->resultReady = false;
            if (m_scan->result.ok) {
                m_apps = std::move(m_scan->result.apps);
                m_scan->result.apps.clear();
                // Labels we already know (cached from earlier sessions).
                for (auto& a : m_apps)
                    if (auto label = m_labelCache->Get(a))
                        a.label = *label;
                // Drop uninstall flags for apps that no longer exist.
                std::erase_if(m_uninstall, [&](const std::string& pkg) {
                    return std::ranges::none_of(m_apps, [&](const AppInfo& a) { return a.package == pkg; });
                });
                changed = true;
            }
        }
    }

    std::vector<std::pair<std::string, std::string>> done;
    {
        std::lock_guard l(m_labels->mutex);
        done.swap(m_labels->done);
    }
    for (auto& [pkg, label] : done) {
        m_labelRequested.erase(pkg);
        if (label.empty())
            continue;
        for (auto& a : m_apps)
            if (a.package == pkg)
                a.label = label;
        changed = true;
    }

    // One low-priority label reader at a time (each pulls a whole base.apk).
    size_t queued;
    {
        std::lock_guard l(m_labels->mutex);
        queued = m_labels->queue.size();
    }
    if (queued > 0 && m_labels->workers.load() == 0 && m_aapt2 && m_adb) {
        ++m_labels->workers;
        auto shared = m_labels;
        auto cache = m_labelCache;
        auto adb = m_adb;
        auto serial = m_serial;
        auto aapt2 = *m_aapt2;
        const auto tmp = Settings::DataDir() / L"tmp";
        jobs.Submit("Reading app labels", JobPriority::Low, [=](Job& job, std::stop_token st) {
            struct Guard {
                LabelShared& s;
                ~Guard() { --s.workers; }
            } guard{ *shared };
            while (!st.stop_requested() && !shared->abandoned) {
                AppInfo app;
                {
                    std::lock_guard l(shared->mutex);
                    if (shared->queue.empty())
                        return;
                    app = std::move(shared->queue.front());
                    shared->queue.pop_front();
                    job.SetDetail(std::format("{} left", shared->queue.size()));
                }
                std::string label = ReadAppLabel(*adb, serial, app, aapt2, tmp, st);
                if (!label.empty())
                    cache->Put(app, label);
                std::lock_guard l(shared->mutex);
                shared->done.push_back({ app.package, std::move(label) });
            }
        });
    }

    if (changed)
        ++m_version;
    return changed;
}

size_t AppSession::UserCount() const { return static_cast<size_t>(std::ranges::count_if(m_apps, [](const AppInfo& a) { return !a.system; })); }

size_t AppSession::FlaggedCount() const
{
    return static_cast<size_t>(std::ranges::count_if(m_apps, [](const AppInfo& a) { return !a.system && a.risk > 0; }));
}

size_t AppSession::HighRiskCount() const
{
    constexpr uint32_t high = AppFlag_Accessibility | AppFlag_DeviceAdmin | AppFlag_Sms | AppFlag_NotifListener | AppFlag_CallLog;
    return static_cast<size_t>(std::ranges::count_if(m_apps, [](const AppInfo& a) { return !a.system && (a.flags & high); }));
}

size_t AppSession::BootCount() const
{
    return static_cast<size_t>(std::ranges::count_if(m_apps, [](const AppInfo& a) { return !a.system && a.Has(AppFlag_Boot); }));
}

void AppSession::SetFlaggedForUninstall(std::string_view pkg, bool on)
{
    if (on)
        m_uninstall.emplace(pkg);
    else if (auto it = m_uninstall.find(pkg); it != m_uninstall.end())
        m_uninstall.erase(it);
    ++m_version;
}

void AppSession::RequestLabel(const AppInfo& app)
{
    if (!m_aapt2 || !app.label.empty() || m_labelRequested.contains(app.package))
        return;
    m_labelRequested.insert(app.package);
    std::lock_guard l(m_labels->mutex);
    m_labels->queue.push_front(app); // on-demand: ahead of a bulk "read all"
}

void AppSession::RequestAllLabels(bool includeSystem)
{
    if (!m_aapt2)
        return;
    std::lock_guard l(m_labels->mutex);
    for (const auto& a : m_apps) {
        if ((a.system && !includeSystem) || !a.label.empty() || m_labelRequested.contains(a.package))
            continue;
        m_labelRequested.insert(a.package);
        m_labels->queue.push_back(a);
    }
}

bool AppSession::LabelPending(std::string_view pkg) const { return m_labelRequested.contains(pkg); }

size_t AppSession::LabelsPending() const { return m_labelRequested.size(); }

} // namespace ps
