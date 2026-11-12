#pragma once
#include "adb/DeviceMonitor.h"
#include "app/AppContext.h"
#include "app/Settings.h"
#include "bg/Background.h"
#include "bg/Usage.h"
#include "report/Exporter.h"
#include "scan/AppSession.h"
#include "scan/FileStore.h"
#include "scan/ScanSession.h"
#include "thumbs/ThumbCache.h"
#include "transfer/Manifest.h"
#include "transfer/Puller.h"
#include "util/JobSystem.h"
#include "update/Update.h"

#include <memory>
#include <string>

namespace ps {

class IPlatform;
class HomeScreen;
class WorkspaceScreen;
class WirelessDialog;

struct AppOptions {
    FakeScenario fake = FakeScenario::None;
    float forcedScale = 0.0f; // >0 overrides DPI + settings (screenshots)
    Route startRoute = Route::Home;
    Page startPage = Page::Overview;
    bool startWithLog = false;
    std::string filesPreset;
    std::string backupRootOverride; // dev: never persisted
    std::string pretendVersion;     // dev: --pretend-version=0.9.0 (test the updater against the real latest release)
    std::string updateKind;         // dev: --update-kind=inno|inno-all|msi|portable (default: detected)
    bool noUpdateCheck = false;     // screenshots: never touch the network
    bool updateSelfTest = false;    // dev: --update-selftest: download + install the latest release unattended
    int openSettingsTab = -1;       // dev: open Settings on this tab
};

// Top-level state machine: owns settings, the adb client, the device monitor,
// and the screens. One Frame() per rendered frame.
class App {
public:
    App(IPlatform& platform, AppOptions opts);
    ~App();

    void Frame();
    // Call when monitor DPI or the UI-scale setting changed.
    void ApplyScale();

private:
    void InitAdb();
    void HandleRequests();
    void StartScan();
    void StartAppScan();
    void UndoAllRestrictions();
    void ConfigureSaveTarget();
    void ApplySaveOptions();
    void HandleSaveRequests();
    void HandleReportRequests();
    ReportInputs MakeReportInputs() const;
    void StartTrimCaches();
    void ConfigureThumbs();
    ThumbConfig MakeThumbConfig() const;
    void DrawSettings();

    IPlatform& m_platform;
    AppOptions m_opts;
    Settings m_settings;
    FileStore m_files;
    ScanSession m_scan;
    AppSession m_apps;
    RestrictionLog m_restrictions{ Settings::DataDir() / L"restrictions.json" };
    VerifiedIndex m_verified;
    UsageStore m_usage;
    JobSystem m_jobs{ 4 }; // declared after what its jobs touch, so it joins first
    SaveQueue m_saveQueue{ m_jobs };
    ThumbCache m_thumbs{ m_platform, m_jobs };
    std::unique_ptr<DeviceMonitor> m_monitor;
    AppContext m_ctx;
    std::string m_scanSerial; // device the FileStore belongs to
    std::string m_scanDeviceId; // its hardware serial (survives USB <-> Wi-Fi)
    std::unique_ptr<WirelessDialog> m_wirelessDlg;
    JobPtr m_trimJob;

    // v1.1 updates
    std::unique_ptr<Updater> m_updater;
    int64_t m_seenCheck = 0;
    bool m_updateAnnounced = false;
    void PumpUpdater();
    bool DrawUpdatesTab();

    // "Auto nuke saved files": verified files waiting to be deleted from the phone.
    struct AutoNuke {
        std::mutex mutex;
        std::vector<std::string> gone; // filled by the job
        bool running = false;
    };
    std::vector<std::string> m_autoNukePending;
    std::shared_ptr<AutoNuke> m_autoNuke = std::make_shared<AutoNuke>();
    void PumpAutoNuke();

    // Cleanup deletions (duplicates / junk).
    struct CleanupDelete {
        std::mutex mutex;
        bool running = false, done = false;
        std::vector<std::string> gone;
        uint64_t freed = 0;
        std::string error, label;
    };
    std::shared_ptr<CleanupDelete> m_cleanup = std::make_shared<CleanupDelete>();
    void PumpCleanupDelete();
    std::unique_ptr<HomeScreen> m_home;
    std::unique_ptr<WorkspaceScreen> m_workspace;
    int m_appliedScalePercent = -1;
    int m_settingsTab = 0;
};

} // namespace ps
