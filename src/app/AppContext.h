#pragma once
#include "adb/DeviceMonitor.h"
#include "app/Settings.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ps {

class IPlatform;
class AdbClient;
class JobSystem;
class FileStore;
class ScanSession;
class ThumbCache;
class AppSession;
class RestrictionLog;
class SaveQueue;
class VerifiedIndex;
class UsageStore;

enum class Route { Home, Workspace };

enum class Page { Overview, Files, Cleanup, Apps, Background, SaveQueue, Nuke, Reports, DataUsage, Count };

// Targeted deletion from the Cleanup page (duplicates, junk).
struct DeleteRequest {
    std::vector<std::string> paths;
    std::vector<std::string> mustExist; // e.g. the kept copy of each duplicate group; abort if any is gone
    std::string label;                  // "duplicate copies", "junk"
};

// Shared state handed to every screen each frame.
struct AppContext {
    IPlatform& platform;
    Settings& settings;
    JobSystem& jobs;
    FileStore& files;
    ScanSession& scan;
    ThumbCache& thumbs;
    AppSession& apps;
    RestrictionLog& restrictions;
    SaveQueue& saveQueue;
    VerifiedIndex& verified;
    UsageStore& usage; // M10: live CPU/RAM + battery per app (Background page fills, Apps / Ask AI read)

    std::shared_ptr<const AdbClient> adb; // null when adb.exe wasn't found
    DeviceSnapshot devices;               // refreshed at the top of every frame

    Route route = Route::Home;
    Page page = Page::Overview;
    std::string activeSerial;
    DeviceInfo activeInfo; // last known info, kept while disconnected

    bool showSettings = false;
    int settingsTab = -1; // open Settings on this tab (5 = Nuke Protector)
    bool showLog = false;
    bool showWireless = false; // M9: Wi-Fi connect dialog

    // Requests from screens, handled by App after the frame.
    bool requestAdbRelocate = false; // "Locate adb.exe…"
    bool requestAdbRetry = false;
    bool requestScan = false;
    bool requestTrimCaches = false;
    bool requestAppScan = false;
    std::string requestForceStop; // package
    bool requestUndoAllRestrictions = false;
    std::vector<uint32_t> requestSaveFiles;   // FileStore indices
    std::vector<std::string> requestSaveApks; // packages
    bool requestSaveKept = false;             // every KEEP file not yet verified
    unsigned requestExport = 0;               // ExportParts bitmask (0 = none)
    bool requestCopyAppList = false;
    std::filesystem::path lastExportDir;      // set by App after an export
    std::optional<DeleteRequest> requestDelete;
    bool deleteRunning = false;               // set by App while a cleanup delete runs
    bool requestReloadVerified = false;       // re-read backup manifests (after zipping)
    std::optional<std::filesystem::path> ffmpeg; // resolved by App (settings override, PATH, winget)
    std::filesystem::path saveFolder;           // today's backup folder for this phone
    int64_t focusFile = -1; // Overview -> Files: index into files to reveal
    std::string filesPreset; // dev flag --files=grid,images,camera,preview (consumed by FilesPage)

    void Navigate(Route r);
    double routeChangedAt = -1.0;

    bool ActiveConnected() const;
    // Stable identity of the active phone (hardware serial), independent of USB vs Wi-Fi.
    // Use for manifests, verified index and restriction records; use activeSerial for adb.
    std::string DeviceId() const;
};

} // namespace ps
