#pragma once
#include "scan/AppScanner.h"
#include "util/JobSystem.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

// App scan of one device + on-demand labels + "flag for uninstall" state.
// Jobs run on the JobSystem; the UI thread calls Pump() each frame.
class AppSession {
public:
    enum class State { Idle, Running, Done, Failed, Cancelled };

    AppSession();
    ~AppSession();

    void Start(JobSystem& jobs, std::shared_ptr<const AdbClient> adb, std::string serial, int deviceSdk);
    void Cancel();
    void Clear();
    // Demo mode (--fake): show a fixed app list without a phone.
    void LoadFake(std::vector<AppInfo> apps);

    // UI thread. Returns true when apps/labels changed.
    bool Pump(JobSystem& jobs);

    State GetState() const;
    bool Running() const { return GetState() == State::Running; }
    AppScanResult Outcome() const; // error info (apps moved out)

    const std::vector<AppInfo>& Apps() const { return m_apps; }
    uint64_t Version() const { return m_version; }
    size_t UserCount() const;
    size_t FlaggedCount() const;  // user apps with any risk flag
    size_t HighRiskCount() const; // user apps with at least one high-severity flag
    size_t BootCount() const;

    // Uninstall intent (Nuke executes it; nothing is uninstalled here).
    bool IsFlaggedForUninstall(std::string_view pkg) const { return m_uninstall.contains(pkg); }
    void SetFlaggedForUninstall(std::string_view pkg, bool on);
    size_t UninstallCount() const { return m_uninstall.size(); }
    const std::set<std::string, std::less<>>& UninstallSet() const { return m_uninstall; }

    // Labels via aapt2.
    bool LabelsSupported() const { return m_aapt2.has_value(); }
    void RequestLabel(const AppInfo& app);
    void RequestAllLabels(bool includeSystem);
    bool LabelPending(std::string_view pkg) const;
    size_t LabelsPending() const;

private:
    struct LabelShared {
        std::mutex mutex;
        std::deque<AppInfo> queue;
        std::vector<std::pair<std::string, std::string>> done;
        std::atomic<int> workers{ 0 };
        std::atomic<bool> abandoned{ false };
    };
    struct ScanShared {
        std::mutex mutex;
        State state = State::Idle;
        AppScanResult result;
        bool resultReady = false;
    };

    std::shared_ptr<ScanShared> m_scan = std::make_shared<ScanShared>();
    std::shared_ptr<LabelShared> m_labels = std::make_shared<LabelShared>();
    std::shared_ptr<LabelCache> m_labelCache;
    std::optional<std::filesystem::path> m_aapt2;
    std::shared_ptr<const AdbClient> m_adb;
    std::string m_serial;
    JobPtr m_job;

    std::vector<AppInfo> m_apps;
    uint64_t m_version = 0;
    std::set<std::string, std::less<>> m_uninstall;
    std::set<std::string, std::less<>> m_labelRequested;
};

} // namespace ps
