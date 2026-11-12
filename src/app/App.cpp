#include "app/App.h"

#include "app/Version.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "platform/Platform.h"
#include "scan/FakeApps.h"
#include "scan/FakeFiles.h"
#include "nuke/Nuker.h"
#include "transfer/LocalPath.h"
#include "util/Strings.h"
#include "screens/HomeScreen.h"
#include "screens/WirelessDialog.h"
#include "screens/WorkspaceScreen.h"
#include "adb/Wireless.h"
#include "bg/Usage.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/Log.h"
#include "util/Utf8.h"
#include "util/Process.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace ps {

using namespace ps::theme;

void AppContext::Navigate(Route r)
{
    if (route == r)
        return;
    route = r;
    routeChangedAt = ImGui::GetTime();
}

std::string AppContext::DeviceId() const
{
    if (auto it = devices.info.find(activeSerial); it != devices.info.end() && !it->second.hwSerial.empty())
        return it->second.hwSerial;
    if (!activeInfo.hwSerial.empty() && activeInfo.serial == activeSerial)
        return activeInfo.hwSerial;
    return activeSerial;
}

bool AppContext::ActiveConnected() const
{
    const DeviceEntry* e = devices.Find(activeSerial);
    return e && e->state == DeviceEntry::State::Device;
}

App::App(IPlatform& platform, AppOptions opts)
    : m_platform(platform)
    , m_opts(opts)
    , m_settings(Settings::Load())
    , m_ctx{ .platform = platform, .settings = m_settings, .jobs = m_jobs, .files = m_files, .scan = m_scan,
          .thumbs = m_thumbs, .apps = m_apps, .restrictions = m_restrictions,
          .saveQueue = m_saveQueue, .verified = m_verified, .usage = m_usage }
    , m_home(std::make_unique<HomeScreen>())
    , m_workspace(std::make_unique<WorkspaceScreen>())
{
    m_wirelessDlg = std::make_unique<WirelessDialog>();
    InitAdb();
    ApplyScale();

    if (opts.startRoute == Route::Workspace) {
        // Screenshot/dev shortcut: jump straight into the first ready device.
        m_ctx.devices = m_monitor->Snapshot();
        if (!m_ctx.devices.info.empty()) {
            const auto& [serial, info] = *m_ctx.devices.info.begin();
            m_ctx.activeSerial = serial;
            m_ctx.activeInfo = info;
            m_ctx.route = Route::Workspace;
            m_ctx.page = opts.startPage;
        }
    }
    m_ctx.showLog = opts.startWithLog;
    m_ctx.filesPreset = opts.filesPreset;
    if (m_ctx.filesPreset == "wifi") { // dev: open the Wi-Fi dialog
        m_ctx.showWireless = true;
        m_ctx.filesPreset.clear();
    }
    if (opts.openSettingsTab >= 0) {
        m_ctx.showSettings = true;
        m_ctx.settingsTab = opts.openSettingsTab;
    }
    if (!opts.backupRootOverride.empty())
        m_settings.backupRoot = opts.backupRootOverride; // in memory only (Save() is only called on settings edits)

    // v1.1 updates: what we are, how we were installed, and a quiet daily check.
    const SemVer current = ParseSemVer(opts.pretendVersion.empty() ? kAppVersion : opts.pretendVersion).value_or(SemVer{});
    const InstallKind kind = ParseInstallKind(opts.updateKind, DetectInstallKind(ExeDir()));
    m_updater = std::make_unique<Updater>(m_jobs, current, kind);
    Updater::CleanDownloads(); // the installer of the update that brought us here
    LogInfo("Version {}, installed as: {}", current.Str(), InstallKindName(kind));
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!opts.pretendVersion.empty() || (m_settings.autoCheckUpdates && !opts.noUpdateCheck && opts.fake == FakeScenario::None && now - m_settings.lastUpdateCheck > 20 * 3600))
        m_updater->Check(true);
}

App::~App() = default;

void App::InitAdb()
{
    m_monitor.reset(); // joins the old poll thread first
    m_ctx.adb.reset();

    if (m_opts.fake != FakeScenario::None) {
        m_monitor = std::make_unique<DeviceMonitor>(m_opts.fake);
        LogInfo("Running with fake device data");
        return;
    }

    std::optional<std::filesystem::path> exe;
    if (!m_settings.adbPathOverride.empty() && std::filesystem::is_regular_file(Widen(m_settings.adbPathOverride)))
        exe = std::filesystem::path(Widen(m_settings.adbPathOverride));
    else
        exe = AdbClient::Locate();

    if (!exe) {
        LogWarn("adb.exe not found (looked next to the exe, ./platform-tools, PATH)");
        m_monitor = std::make_unique<DeviceMonitor>(FakeScenario::NoAdb);
        return;
    }
    auto client = std::make_shared<AdbClient>(*exe);
    m_ctx.adb = client;
    m_monitor = std::make_unique<DeviceMonitor>(client, std::chrono::milliseconds(std::clamp(m_settings.devicePollMs, 500, 10000)));

    // M9: reconnect phones used over Wi-Fi before (quietly; offline ones just fail).
    if (m_settings.wirelessReconnect && !m_settings.wirelessKnown.empty()) {
        std::vector<std::string> addresses;
        for (const auto& [a, name] : m_settings.wirelessKnown)
            addresses.push_back(a);
        m_jobs.Submit("Reconnecting Wi-Fi phones", JobPriority::Low, [client, addresses](Job&, std::stop_token st) {
            for (const auto& a : addresses)
                if (!st.stop_requested())
                    ConnectWireless(*client, a, st);
        });
    }
}

void App::ApplyScale()
{
    const float scale = m_opts.forcedScale > 0.0f ? m_opts.forcedScale
        : m_settings.uiScalePercent > 0           ? m_settings.uiScalePercent / 100.0f
                                                  : m_platform.DpiScale();
    theme::Apply(scale);
    m_appliedScalePercent = m_settings.uiScalePercent;
}

void App::HandleRequests()
{
    if (m_ctx.requestReloadVerified) {
        m_ctx.requestReloadVerified = false;
        if (!m_ctx.activeSerial.empty())
            m_verified.LoadForDevice(m_settings.BackupRoot(), m_ctx.DeviceId());
    }
    if (m_ctx.requestAdbRetry) {
        m_ctx.requestAdbRetry = false;
        InitAdb();
        if (m_ctx.adb)
            ui::Toast("adb found");
        else
            ui::Toast("Still no adb.exe", ui::ToastKind::Warning);
    }
    if (m_ctx.requestAdbRelocate) {
        m_ctx.requestAdbRelocate = false;
        if (auto path = m_platform.OpenFileDialog(L"adb.exe", L"adb.exe")) {
            m_settings.adbPathOverride = Narrow(path->wstring());
            m_settings.Save();
            InitAdb();
        }
    }
    if (m_appliedScalePercent != m_settings.uiScalePercent)
        ApplyScale();

    if (m_ctx.requestScan) {
        m_ctx.requestScan = false;
        StartScan();
    }
    if (m_ctx.requestTrimCaches) {
        m_ctx.requestTrimCaches = false;
        StartTrimCaches();
    }
    if (m_ctx.requestAppScan) {
        m_ctx.requestAppScan = false;
        StartAppScan();
    }
    if (m_ctx.requestUndoAllRestrictions) {
        m_ctx.requestUndoAllRestrictions = false;
        UndoAllRestrictions();
    }
    HandleSaveRequests();
    HandleReportRequests();
    PumpCleanupDelete();
    if (!m_ctx.requestForceStop.empty()) {
        const std::string pkg = std::exchange(m_ctx.requestForceStop, {});
        if (m_ctx.adb && m_ctx.ActiveConnected()) {
            auto adb = m_ctx.adb;
            const std::string serial = m_ctx.activeSerial;
            m_jobs.Submit("Force-stopping app", JobPriority::High, [adb, serial, pkg](Job& job, std::stop_token st) {
                const auto r = adb->Shell(serial, ShellCommand("am").arg("force-stop").arg(pkg), { .timeout = 20s, .cancel = st });
                if (!r.ok())
                    job.Fail(r.err.empty() ? std::string("am force-stop failed") : r.err);
            });
            ui::Toast("Force-stopped " + pkg);
        }
    }
}

ReportInputs App::MakeReportInputs() const
{
    ReportInputs in;
    in.device = m_ctx.activeInfo;
    in.maskSerial = m_settings.reportMaskSerial;
    in.fullPermissions = m_settings.reportFullPermissions;
    in.files = &m_files;
    in.apps = &m_apps.Apps();
    in.verified = &m_verified;
    const auto o = m_scan.Outcome();
    in.permissionDenied = o.permissionDenied;
    in.unreadableNames = o.unreadableNames;
    in.sessionLog = Log::Get().FilePath();
    return in;
}

void App::HandleReportRequests()
{
    if (m_ctx.requestCopyAppList) {
        m_ctx.requestCopyAppList = false;
        if (m_apps.Apps().empty()) {
            ui::Toast("Scan apps first", ui::ToastKind::Warning);
        } else {
            m_platform.SetClipboardText(FormatUserAppList(MakeReportInputs()));
            ui::Toast(std::format("Copied {} apps for your AI assistant", m_apps.UserCount()));
        }
    }
    if (m_ctx.requestExport) {
        const unsigned parts = std::exchange(m_ctx.requestExport, 0u);
        if (m_files.Files().empty() && m_apps.Apps().empty()) {
            ui::Toast("Scan the phone before exporting", ui::ToastKind::Warning);
            return;
        }
        const auto r = ExportReports(MakeReportInputs(), m_settings.ReportsRoot(), parts);
        if (r.ok) {
            m_ctx.lastExportDir = r.dir;
            ui::Toast(std::format("Exported {} files to reports\\{}", r.files.size(), Narrow(r.dir.filename().wstring())));
            m_ctx.page = Page::Reports;
        } else {
            ui::Toast("Export failed: " + r.error, ui::ToastKind::Error);
        }
    }
}

void App::PumpCleanupDelete()
{
    // Finished job -> update the file list and report.
    {
        std::lock_guard l(m_cleanup->mutex);
        if (m_cleanup->done) {
            m_cleanup->done = false;
            m_cleanup->running = false;
            if (!m_cleanup->error.empty())
                ui::Toast(m_cleanup->error, ui::ToastKind::Error);
            else
                ui::Toast(std::format("Deleted {} {} \xC2\xB7 freed {}", m_cleanup->gone.size(), m_cleanup->label, FormatBytes(m_cleanup->freed)));
            m_files.RemovePaths(m_cleanup->gone);
            m_cleanup->gone.clear();
        }
        m_ctx.deleteRunning = m_cleanup->running;
    }
    if (!m_ctx.requestDelete)
        return;
    DeleteRequest req = std::move(*m_ctx.requestDelete);
    m_ctx.requestDelete.reset();
    if (!m_ctx.adb || !m_ctx.ActiveConnected() || req.paths.empty())
        return;
    {
        std::lock_guard l(m_cleanup->mutex);
        if (m_cleanup->running)
            return;
        m_cleanup->running = true;
        m_cleanup->error.clear();
        m_cleanup->label = req.label;
    }
    // Sizes for the "freed" message.
    std::unordered_map<std::string_view, uint64_t> sizes;
    for (const auto& e : m_files.Files())
        sizes.emplace(e.path, e.Disk());
    uint64_t planned = 0;
    for (const auto& p : req.paths)
        if (auto it = sizes.find(p); it != sizes.end())
            planned += it->second;

    auto shared = m_cleanup;
    auto adb = m_ctx.adb;
    const std::string serial = m_ctx.activeSerial;
    const auto tmp = Settings::DataDir() / L"tmp";
    std::unordered_map<std::string, uint64_t> sizeOf;
    for (const auto& p : req.paths)
        if (auto it = sizes.find(p); it != sizes.end())
            sizeOf[p] = it->second;
    LogWarn("Cleanup: deleting {} {} ({} bytes)", req.paths.size(), req.label, planned);
    m_jobs.Submit("Cleaning up", JobPriority::High, [shared, adb, serial, tmp, req = std::move(req), sizeOf](Job& job, std::stop_token st) {
        // Safety: every copy we promised to keep must still be on the phone.
        for (size_t i = 0; i < req.mustExist.size(); i += 100) {
            ShellCommand ls("ls");
            ls.arg("-d").arg("--");
            const size_t n = std::min<size_t>(100, req.mustExist.size() - i);
            for (size_t k = 0; k < n; ++k)
                ls.arg(req.mustExist[i + k]);
            const auto r = adb->Shell(serial, ls.raw("2>/dev/null"), { .timeout = 60s, .cancel = st });
            size_t lines = 0;
            ForEachLine(r.out, [&](std::string_view l) { lines += !l.empty(); });
            if (lines != n) {
                std::lock_guard l(shared->mutex);
                shared->error = "Stopped: a copy that should be kept is no longer on the phone. Rescan, then try again.";
                shared->done = true;
                LogError("Cleanup aborted: kept copies missing ({} of {} found)", lines, n);
                return;
            }
        }
        job.SetDetail(std::format("{} files", req.paths.size()));
        auto gone = DeleteFiles(*adb, serial, req.paths, tmp, st);
        CleanMediaStore(*adb, serial, gone, st);
        uint64_t freed = 0;
        for (const auto& p : gone)
            if (auto it = sizeOf.find(p); it != sizeOf.end())
                freed += it->second;
        std::lock_guard l(shared->mutex);
        shared->gone = std::move(gone);
        shared->freed = freed;
        shared->done = true;
    });
}

void App::PumpAutoNuke()
{
    // Collect results of the last batch.
    std::vector<std::string> gone;
    bool running;
    {
        std::lock_guard l(m_autoNuke->mutex);
        gone.swap(m_autoNuke->gone);
        running = m_autoNuke->running;
    }
    if (!gone.empty()) {
        m_files.RemovePaths(gone);
        ui::Toast(std::format("Moved {} files to the PC (deleted from the phone)", gone.size()));
    }
    if (running || m_autoNukePending.empty() || !m_ctx.adb || !m_ctx.ActiveConnected())
        return;
    // Batch: let a few verifications accumulate, but never wait long.
    std::vector<std::string> batch;
    batch.swap(m_autoNukePending);
    {
        std::lock_guard l(m_autoNuke->mutex);
        m_autoNuke->running = true;
    }
    auto shared = m_autoNuke;
    auto adb = m_ctx.adb;
    const std::string serial = m_ctx.activeSerial;
    const auto tmp = Settings::DataDir() / L"tmp";
    m_jobs.Submit("Removing saved files from the phone", JobPriority::Normal, [shared, adb, serial, batch, tmp](Job& job, std::stop_token st) {
        job.SetDetail(std::format("{} files", batch.size()));
        LogWarn("Auto nuke (move mode): deleting {} verified files from the phone", batch.size());
        auto removed = DeleteFiles(*adb, serial, batch, tmp, st);
        std::lock_guard l(shared->mutex);
        shared->gone.insert(shared->gone.end(), removed.begin(), removed.end());
        shared->running = false;
    });
}

void App::ApplySaveOptions()
{
    SaveOptions o;
    o.verifyMd5 = m_settings.verifyMd5;
    o.parallel = m_settings.parallelTransfers;
    o.conflict = static_cast<ConflictPolicy>(std::clamp(m_settings.conflictPolicy, 0, 2));
    m_saveQueue.SetOptions(o);
}

void App::ConfigureSaveTarget()
{
    if (m_ctx.activeSerial.empty())
        return;
    // One folder per device per day: <root>/<pattern>, e.g. Documents/PhoneSweep/SM-G960F_2026-11-11.
    const auto root = m_settings.BackupRoot();
    const std::string device = m_ctx.activeInfo.model.empty() ? m_ctx.activeSerial : m_ctx.activeInfo.model;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto folder = root / Widen(ExpandFolderPattern(m_settings.folderPattern, device, now));
    m_ctx.saveFolder = folder;
    m_saveQueue.SetTarget(m_ctx.adb, m_ctx.activeSerial, m_ctx.DeviceId(), m_ctx.activeInfo.model, folder);
    ApplySaveOptions();
    m_verified.LoadForDevice(root, m_ctx.DeviceId());
}

void App::HandleSaveRequests()
{
    const bool any = !m_ctx.requestSaveFiles.empty() || !m_ctx.requestSaveApks.empty() || m_ctx.requestSaveKept;
    if (!any)
        return;
    if (!m_ctx.adb) {
        m_ctx.requestSaveFiles.clear();
        m_ctx.requestSaveApks.clear();
        m_ctx.requestSaveKept = false;
        ui::Toast("Saving needs a real phone (adb)", ui::ToastKind::Warning);
        return;
    }
    ApplySaveOptions();

    std::vector<SaveRequest> reqs;
    uint64_t bytes = 0;
    size_t alreadySaved = 0;
    const auto& files = m_files.Files();
    const auto& vols = m_files.Volumes();
    auto addFile = [&](uint32_t idx) {
        if (idx >= files.size())
            return;
        const FileEntry& e = files[idx];
        if (m_verified.IsVerified(e.path, e.size, e.mtime)) {
            ++alreadySaved;
            return;
        }
        SaveRequest r;
        r.devicePath = e.path;
        r.size = e.size;
        r.mtime = e.mtime;
        const std::string prefix = e.volume > 0 && e.volume < vols.size() ? vols[e.volume].label : std::string();
        r.localRel = MapToLocal(prefix, e.RelPath(), m_settings.preserveStructure);
        bytes += e.size;
        reqs.push_back(std::move(r));
    };
    for (uint32_t idx : m_ctx.requestSaveFiles)
        addFile(idx);
    if (m_ctx.requestSaveKept)
        for (uint32_t i = 0; i < files.size(); ++i)
            if (files[i].flags & FileFlag_Keep)
                addFile(i);
    for (const auto& pkg : m_ctx.requestSaveApks) {
        for (const auto& a : m_apps.Apps()) {
            if (a.package != pkg)
                continue;
            for (const auto& apk : a.apkPaths) {
                SaveRequest r;
                r.devicePath = apk;
                r.kind = "apk";
                const auto name = apk.substr(apk.rfind('/') + 1);
                r.localRel = std::filesystem::path(L"apks") / Widen(SanitizeComponent(pkg)) / Widen(SanitizeComponent(name));
                reqs.push_back(std::move(r));
            }
        }
    }
    const size_t apkCount = m_ctx.requestSaveApks.size();
    m_ctx.requestSaveFiles.clear();
    m_ctx.requestSaveApks.clear();
    m_ctx.requestSaveKept = false;

    const size_t added = m_saveQueue.Enqueue(std::move(reqs));
    if (added == 0 && alreadySaved > 0)
        ui::Toast(std::format("All {} files are already saved and verified", alreadySaved));
    else if (added == 0)
        ui::Toast("Nothing new to save", ui::ToastKind::Info);
    else if (apkCount && bytes == 0)
        ui::Toast(std::format("Backing up {} app{} to the PC", apkCount, apkCount > 1 ? "s" : ""));
    else
        ui::Toast(std::format("Queued {} files \xC2\xB7 {}{}", added, FormatBytes(bytes),
            alreadySaved ? std::format(" ({} already saved)", alreadySaved) : std::string()));
    LogInfo("Save queue: +{} items into {}", added, Narrow(m_saveQueue.Folder().wstring()));
}

void App::UndoAllRestrictions()
{
    if (!m_ctx.adb || !m_ctx.ActiveConnected())
        return;
    const auto entries = m_restrictions.ForDevice(m_ctx.DeviceId());
    const std::string serial = m_ctx.activeSerial;
    if (entries.empty())
        return;
    auto adb = m_ctx.adb;
    auto* log = &m_restrictions;
    m_jobs.Submit("Undoing background restrictions", JobPriority::High, [adb, log, entries, serial](Job& job, std::stop_token st) {
        size_t ok = 0;
        for (const auto& r : entries) {
            job.SetDetail(r.package);
            std::string err;
            if (UndoRestriction(*adb, serial, r, &err, st)) {
                log->Remove(r.serial, r.package);
                ++ok;
            } else {
                LogWarn("Undo failed for {}: {}", r.package, err);
            }
        }
        job.SetDetail(std::format("{} of {} restored", ok, entries.size()));
    });
    ui::Toast(std::format("Undoing {} background restrictions", entries.size()), ui::ToastKind::Info);
}

void App::StartAppScan()
{
    if (!m_ctx.adb || !m_ctx.ActiveConnected())
        return;
    m_apps.Start(m_jobs, m_ctx.adb, m_ctx.activeSerial, m_ctx.activeInfo.sdk);
}

void App::StartScan()
{
    if (!m_ctx.adb || !m_ctx.ActiveConnected()) {
        ui::Toast("Connect the phone to scan", ui::ToastKind::Warning);
        return;
    }
    ScanOptions opts;
    opts.includeSdCard = m_settings.includeSdCard;
    opts.minSizeBytes = static_cast<uint64_t>(std::max(0, m_settings.minFileSizeKB)) * 1024;
    m_scanSerial = m_ctx.activeSerial;
    m_scan.Start(m_jobs, m_ctx.adb, m_ctx.activeSerial, opts);
}

ThumbConfig App::MakeThumbConfig() const
{
    ThumbConfig cfg;
    cfg.cacheDir = Settings::DataDir() / L"thumbs";
    if (!m_settings.ffmpegPathOverride.empty() && std::filesystem::is_regular_file(Widen(m_settings.ffmpegPathOverride)))
        cfg.ffmpeg = std::filesystem::path(Widen(m_settings.ffmpegPathOverride));
    else
        cfg.ffmpeg = FindExecutable(L"ffmpeg.exe");
    return cfg;
}

void App::ConfigureThumbs()
{
    m_ctx.ffmpeg = MakeThumbConfig().ffmpeg; // also used by Cleanup -> Compress
    if (!m_ctx.adb || m_ctx.activeSerial.empty()) {
        m_thumbs.Reset();
        return;
    }
    ThumbConfig cfg = MakeThumbConfig();
    LogInfo("Thumbnails: cache {}, ffmpeg {}", Narrow(cfg.cacheDir.wstring()),
        cfg.ffmpeg ? Narrow(cfg.ffmpeg->wstring()) : std::string("not found (videos get an icon)"));
    m_thumbs.SetSource(m_ctx.adb, m_ctx.activeSerial, std::move(cfg));

    // Keep the disk cache under its limit (cheap; runs in the background).
    const auto dir = Settings::DataDir() / L"thumbs";
    const uint64_t limit = static_cast<uint64_t>(std::max(64, m_settings.thumbCacheMB)) * 1'000'000;
    m_jobs.Submit("Trimming thumbnail cache", JobPriority::Low, [dir, limit](Job&, std::stop_token) { TrimThumbDisk(dir, limit); });
}

void App::StartTrimCaches()
{
    if (!m_ctx.adb || !m_ctx.ActiveConnected() || (m_trimJob && !m_trimJob->Finished()))
        return;
    auto adb = m_ctx.adb;
    const std::string serial = m_ctx.activeSerial;
    m_trimJob = m_jobs.Submit("Clearing app caches", JobPriority::Normal, [adb, serial](Job& job, std::stop_token st) {
        auto freeBytes = [&]() -> std::optional<uint64_t> {
            const auto r = adb->Shell(serial, ShellCommand("df").arg("-k").arg("/data"), { .timeout = 10s, .cancel = st });
            if (auto s = ParseDfK(r.out))
                return s->free;
            return std::nullopt;
        };
        const auto before = freeBytes();
        job.SetDetail("pm trim-caches");
        // Asking for an absurd amount makes Android clear every cache it can.
        const auto r = adb->Shell(serial, ShellCommand("pm").arg("trim-caches").arg("999G"), { .timeout = 120s, .cancel = st });
        if (!r.ok()) {
            job.Fail(r.err.empty() ? std::string("pm trim-caches failed") : r.err);
            return;
        }
        const auto after = freeBytes();
        const int64_t freed = (before && after) ? static_cast<int64_t>(*after) - static_cast<int64_t>(*before) : 0;
        job.SetDetail(freed > 0 ? "Cache cleared: " + FormatBytes(static_cast<uint64_t>(freed)) + " freed" : "Caches cleared");
        LogInfo("pm trim-caches: freed {} bytes", freed);
    });
}

void App::Frame()
{
    m_ctx.devices = m_monitor->Snapshot();
    ui::SetBinaryUnits(m_settings.binaryUnits);

    // --workspace with a real phone: enter as soon as the first device is ready.
    if (m_opts.startRoute == Route::Workspace && m_ctx.route == Route::Home && !m_ctx.devices.info.empty()) {
        const auto& [serial, info] = *m_ctx.devices.info.begin();
        m_ctx.activeSerial = serial;
        m_ctx.activeInfo = info;
        m_ctx.route = Route::Workspace;
        m_ctx.page = m_opts.startPage;
        m_opts.startRoute = Route::Home; // one-shot
    }
    m_scan.Pump(m_files);
    m_apps.Pump(m_jobs);
    PumpUpdater();

    // M9: the cable was pulled but the same phone is still here over Wi-Fi (or the
    // other way round): follow it instead of showing "disconnected".
    if (m_ctx.route == Route::Workspace && !m_ctx.activeSerial.empty() && !m_ctx.ActiveConnected() && !m_ctx.activeInfo.hwSerial.empty()) {
        for (const auto& [serial, info] : m_ctx.devices.info) {
            const DeviceEntry* e = m_ctx.devices.Find(serial);
            if (serial != m_ctx.activeSerial && info.hwSerial == m_ctx.activeInfo.hwSerial && e && e->state == DeviceEntry::State::Device) {
                LogInfo("Same phone now on {} ({}), switching over", serial, info.wireless ? "Wi-Fi" : "USB");
                ui::Toast(info.wireless ? "Cable unplugged \xE2\x80\x94 continuing over Wi-Fi" : "Continuing over USB", ui::ToastKind::Info);
                m_ctx.activeSerial = serial;
                m_ctx.activeInfo = info;
                break;
            }
        }
    }
    // Same phone, different link (USB <-> Wi-Fi): keep the scan, just re-point adb users.
    if (m_ctx.route == Route::Workspace && m_ctx.activeSerial != m_scanSerial && !m_scanDeviceId.empty() && m_ctx.DeviceId() == m_scanDeviceId) {
        m_scanSerial = m_ctx.activeSerial;
        ConfigureThumbs();
        ConfigureSaveTarget();
    }
    // Entering a different device's workspace: drop the old scan, auto-scan the new one.
    if (m_ctx.route == Route::Workspace && m_ctx.activeSerial != m_scanSerial) {
        m_scanDeviceId = m_ctx.DeviceId();
        m_usage.Clear();
        m_scan.Cancel();
        m_files.Clear();
        m_scanSerial = m_ctx.activeSerial;
        ConfigureThumbs();
        ConfigureSaveTarget();
        if (m_opts.fake != FakeScenario::None) {
            m_files.SetVolumes(FakeVolumes());
            m_files.Append(FakeFiles(4000));
            m_apps.LoadFake(FakeApps());
        } else if (m_settings.autoScan && m_ctx.ActiveConnected()) {
            m_ctx.requestScan = true;
            m_ctx.requestAppScan = true;
        }
    }

    m_thumbs.SetEnabled(m_settings.showThumbnails);
    m_thumbs.BeginFrame();

    // Save queue: follow the connection, run workers, feed verified files to the index.
    if (m_ctx.route == Route::Workspace) {
        m_saveQueue.SetDeviceConnected(m_ctx.ActiveConnected());
        m_saveQueue.Tick();
        const auto folder = m_saveQueue.Folder();
        for (const auto& e : m_saveQueue.DrainVerified()) {
            m_verified.Add(e, folder);
            // Move mode: verified files leave the phone. APK backups never do.
            if (m_settings.autoNukeSaved && e.kind == "file")
                m_autoNukePending.push_back(e.devicePath);
        }
        PumpAutoNuke();
    }

    // Report finished cache clearing.
    if (m_trimJob && m_trimJob->Finished()) {
        if (m_trimJob->GetState() == Job::State::Failed)
            ui::Toast("Couldn't clear caches: " + m_trimJob->Error(), ui::ToastKind::Error);
        else if (m_trimJob->GetState() == Job::State::Done)
            ui::Toast(m_trimJob->Detail());
        m_monitor->RequestInfoRefresh(m_ctx.activeSerial);
        m_trimJob.reset();
    }

    // 150 ms fade-in after a route change.
    const double since = ImGui::GetTime() - m_ctx.routeChangedAt;
    const float alpha = m_ctx.routeChangedAt < 0 ? 1.0f : static_cast<float>(std::clamp(since / 0.15, 0.0, 1.0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(alpha, 0.01f));

    if (m_ctx.route == Route::Home)
        m_home->Draw(m_ctx);
    else
        m_workspace->Draw(m_ctx);

    ImGui::PopStyleVar();

    m_wirelessDlg->Draw(m_ctx);
    if (m_ctx.showSettings)
        DrawSettings();

    ui::DrawToasts(m_ctx.route == Route::Workspace ? S(StatusBarHeight) : S(40));
    HandleRequests();
}

void App::DrawSettings()
{
    if (!ImGui::IsPopupOpen("Settings"))
        ImGui::OpenPopup("Settings");

    const ImVec2 size(std::min(S(760), ImGui::GetMainViewport()->WorkSize.x - S(40)),
        std::min(S(520), ImGui::GetMainViewport()->WorkSize.y - S(40)));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = true;
    if (!ImGui::BeginPopupModal("Settings", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        m_ctx.showSettings = false;
        return;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    static constexpr const char* kTabs[] = { "General", "Backup", "Scan", "Thumbnails", "ADB", "Nuke Protector", "Reports", "Updates", "About" };
    if (m_ctx.settingsTab >= 0) {
        m_settingsTab = std::exchange(m_ctx.settingsTab, -1);
    }

    // Left tab column.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(14)));
    ImGui::BeginChild("##settabs", ImVec2(S(170), 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        Font f(GetFonts().uiStrong, FontH2 - 4);
        ImGui::SetCursorPosX(S(10));
        ImGui::TextUnformatted("Settings");
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    for (int i = 0; i < static_cast<int>(std::size(kTabs)); ++i) {
        const bool active = m_settingsTab == i;
        ImGui::PushStyleColor(ImGuiCol_Header, PanelRaised);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Text : TextDim);
        if (ImGui::Selectable(kTabs[i], active, 0, ImVec2(0, S(28))))
            m_settingsTab = i;
        ImGui::PopStyleColor(2);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(18)));
    ImGui::BeginChild("##setbody", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        Font f(GetFonts().uiStrong, FontBase + 2);
        ImGui::TextUnformatted(kTabs[m_settingsTab]);
    }
    // Close button, top-right.
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(28));
    if (ui::GhostButton(ICON_XMARK "##close"))
        open = false;
    ImGui::Dummy(ImVec2(0, S(6)));

    bool changed = false;
    switch (m_settingsTab) {
    case 0: { // General
        changed |= ImGui::Checkbox("Auto-scan when a phone is selected", &m_settings.autoScan);
        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("Units");
        int units = m_settings.binaryUnits ? 1 : 0;
        changed |= ImGui::RadioButton("GB (decimal)", &units, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("GiB (binary)", &units, 1);
        m_settings.binaryUnits = units == 1;
        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("UI scale");
        static constexpr int kScales[] = { 0, 100, 125, 150 };
        static constexpr const char* kScaleNames[] = { "Auto", "100%", "125%", "150%" };
        for (int i = 0; i < 4; ++i) {
            if (i)
                ImGui::SameLine();
            if (ImGui::RadioButton(kScaleNames[i], m_settings.uiScalePercent == kScales[i])) {
                m_settings.uiScalePercent = kScales[i];
                changed = true;
            }
        }
        if (m_settings.uiScalePercent == 0) {
            Font sf(GetFonts().ui, FontSmall);
            ui::DimText(std::format("Following monitor DPI ({:.0f}%).", m_platform.DpiScale() * 100.0f));
        }
        break;
    }
    case 4: { // ADB
        ui::DimText("adb.exe in use");
        {
            Font mf(GetFonts().mono, FontSmall);
            ImGui::TextWrapped("%s", m_ctx.devices.adbFound ? m_ctx.devices.adbPath.c_str() : "not found");
        }
        if (!m_ctx.devices.adbVersion.empty())
            ui::DimText("Version " + m_ctx.devices.adbVersion);
        ImGui::Dummy(ImVec2(0, S(4)));
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Browse\xE2\x80\xA6"))
            m_ctx.requestAdbRelocate = true;
        if (!m_settings.adbPathOverride.empty()) {
            ImGui::SameLine();
            if (ui::SecondaryButton("Use auto-detect")) {
                m_settings.adbPathOverride.clear();
                changed = true;
                m_ctx.requestAdbRetry = true;
            }
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::SetNextItemWidth(S(220));
        changed |= ImGui::SliderInt("Device poll interval", &m_settings.devicePollMs, 500, 5000, "%d ms");
        Font sf(GetFonts().ui, FontSmall);
        ui::DimText("Applies after the next adb restart (Retry, or relaunch).");
        break;
    }
    case 1: { // Backup
        const auto root = m_settings.BackupRoot();
        ui::DimText("Backup location");
        {
            Font mf(GetFonts().mono, FontSmall);
            ImGui::TextWrapped("%s", Narrow(root.wstring()).c_str());
        }
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Change\xE2\x80\xA6##root")) {
            if (auto p = m_platform.PickFolder(root)) {
                m_settings.backupRoot = Narrow(p->wstring());
                changed = true;
                ConfigureSaveTarget();
            }
        }
        ImGui::SameLine();
        if (ui::SecondaryButton(ICON_EXTERNAL "  Open##root")) {
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            m_platform.OpenInExplorer(root);
        }
        if (!m_settings.backupRoot.empty()) {
            ImGui::SameLine();
            if (ui::GhostButton("Use default")) {
                m_settings.backupRoot.clear();
                changed = true;
                ConfigureSaveTarget();
            }
        }

        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("Folder naming");
        static char pattern[128] = {};
        if (pattern[0] == 0)
            strncpy_s(pattern, m_settings.folderPattern.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::InputText("##pattern", pattern, sizeof(pattern))) {
            m_settings.folderPattern = pattern[0] ? pattern : "{device}_{date}";
            changed = true;
        }
        {
            Font sf(GetFonts().ui, FontSmall);
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const std::string device = m_ctx.activeInfo.model.empty() ? std::string("SM-G960F") : m_ctx.activeInfo.model;
            ui::DimText(std::format("Preview: {}  \xC2\xB7  tokens: {{device}} {{date}} {{time}}", ExpandFolderPattern(m_settings.folderPattern, device, now)));
        }

        ImGui::Dummy(ImVec2(0, S(6)));
        changed |= ImGui::Checkbox("Preserve the phone's folder structure", &m_settings.preserveStructure);

        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("Verification");
        int verify = m_settings.verifyMd5 ? 1 : 0;
        changed |= ImGui::RadioButton("Size only (faster)", &verify, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Size + MD5 (recommended)", &verify, 1);
        m_settings.verifyMd5 = verify == 1;

        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::SetNextItemWidth(S(200));
        changed |= ImGui::SliderInt("Parallel transfers", &m_settings.parallelTransfers, 1, 4);

        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("If a file already exists");
        static constexpr const char* kPolicies[] = { "Skip if identical", "Keep both (rename)", "Overwrite" };
        for (int i = 0; i < 3; ++i) {
            if (i)
                ImGui::SameLine();
            if (ImGui::RadioButton(kPolicies[i], m_settings.conflictPolicy == i)) {
                m_settings.conflictPolicy = i;
                changed = true;
            }
        }

        ImGui::Dummy(ImVec2(0, S(6)));
        changed |= ImGui::Checkbox("Back up APKs of flagged apps before uninstalling", &m_settings.backupApksBeforeUninstall);
        {
            Font sf(GetFonts().ui, FontSmall);
            ImGui::Dummy(ImVec2(0, S(4)));
            ui::DimText(ICON_INFO "  Zip backups arrive after Milestone 4. Photos and videos barely compress anyway.");
        }
        if (changed)
            ApplySaveOptions();
        break;
    }
    case 2: { // Scan
        changed |= ImGui::Checkbox("Include SD card", &m_settings.includeSdCard);
        {
            Font sf(GetFonts().ui, FontSmall);
            ui::DimText("Scans removable storage too (camera photos often live there).");
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("Ignore files smaller than");
        static constexpr int kMin[] = { 0, 10, 100 };
        static constexpr const char* kMinNames[] = { "Nothing", "10 KB", "100 KB" };
        for (int i = 0; i < 3; ++i) {
            if (i)
                ImGui::SameLine();
            if (ImGui::RadioButton(kMinNames[i], m_settings.minFileSizeKB == kMin[i])) {
                m_settings.minFileSizeKB = kMin[i];
                changed = true;
            }
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        ui::DimText("Protected folders");
        ImGui::TextUnformatted(ICON_LOCK "  Android/data   " ICON_LOCK "  Android/obb   " ICON_LOCK "  hidden (dot) folders");
        {
            Font sf(GetFonts().ui, FontSmall);
            ui::DimText("Listed but greyed out, and never touched by Nuke. Reveal them with \"Show protected\" in Files.");
            ImGui::Dummy(ImVec2(0, S(6)));
            ui::DimText("Changes apply to the next scan.");
        }
        break;
    }
    case 3: { // Thumbnails
        changed |= ImGui::Checkbox("Show thumbnails", &m_settings.showThumbnails);
        ImGui::Dummy(ImVec2(0, S(6)));

        ui::DimText("Disk cache limit");
        static constexpr int kSizes[] = { 256, 512, 1024, 2048 };
        static constexpr const char* kNames[] = { "256 MB", "512 MB", "1 GB", "2 GB" };
        for (int i = 0; i < 4; ++i) {
            if (i)
                ImGui::SameLine();
            if (ImGui::RadioButton(kNames[i], m_settings.thumbCacheMB == kSizes[i])) {
                m_settings.thumbCacheMB = kSizes[i];
                changed = true;
            }
        }
        // Disk usage is a directory walk: refresh at most every 2 s while this tab is open.
        static double lastCheck = -10.0;
        static uint64_t usage = 0;
        const auto dir = Settings::DataDir() / L"thumbs";
        if (ImGui::GetTime() - lastCheck > 2.0) {
            usage = ThumbDiskUsage(dir);
            lastCheck = ImGui::GetTime();
        }
        ui::DimText(std::format("Using {} in {}", ui::Bytes(usage), Narrow(dir.wstring())));
        if (ui::SecondaryButton(ICON_TRASH "  Clear cache")) {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec))
                    std::filesystem::remove(it->path(), ec);
            lastCheck = -10.0;
            ui::Toast("Thumbnail cache cleared");
        }

        ImGui::Dummy(ImVec2(0, S(10)));
        ui::DimText("ffmpeg (video thumbnails)");
        const auto cfg = MakeThumbConfig();
        {
            Font mf(GetFonts().mono, FontSmall);
            ImGui::TextWrapped("%s", cfg.ffmpeg ? Narrow(cfg.ffmpeg->wstring()).c_str() : "not found");
        }
        if (!cfg.ffmpeg) {
            Font sf(GetFonts().ui, FontSmall);
            ui::DimText("Without ffmpeg, videos show an icon. Put ffmpeg.exe on PATH or next to PhoneSweep.exe.");
        }
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Browse\xE2\x80\xA6##ffmpeg")) {
            if (auto p = m_platform.OpenFileDialog(L"ffmpeg.exe", L"ffmpeg.exe")) {
                m_settings.ffmpegPathOverride = Narrow(p->wstring());
                changed = true;
                ConfigureThumbs();
            }
        }
        if (!m_settings.ffmpegPathOverride.empty()) {
            ImGui::SameLine();
            if (ui::SecondaryButton("Use auto-detect##ffmpeg")) {
                m_settings.ffmpegPathOverride.clear();
                changed = true;
                ConfigureThumbs();
            }
        }
        break;
    }
    case 5: { // Nuke Protector
        ui::DimText("Protection mode");
        auto modeCard = [&](int mode, const char* icon, const char* title, const char* text, const ImVec4& col) {
            const bool active = m_settings.nukeMode == mode;
            ImGui::PushID(mode);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = S(64);
            if (ImGui::InvisibleButton("##mode", ImVec2(w, h)) && !active) {
                m_settings.nukeMode = mode;
                changed = true;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(active ? Mix(PanelRaised, col, 0.08f) : PanelRaised), S(RoundPanel));
            dl->AddRect(p, ImVec2(p.x + w, p.y + h), U32(active ? col : Border), S(RoundPanel), 0, active ? 1.5f : 1.0f);
            // Radio dot.
            const ImVec2 c(p.x + S(18), p.y + h * 0.5f);
            dl->AddCircle(c, S(7), U32(active ? col : TextDim), 0, 1.5f);
            if (active)
                dl->AddCircleFilled(c, S(3.5f), U32(col));
            {
                Font f(GetFonts().uiStrong, FontBase);
                dl->AddText(ImVec2(p.x + S(36), p.y + S(12)), U32(active ? col : Text), (std::string(icon) + "  " + title).c_str());
            }
            {
                Font f(GetFonts().ui, FontSmall);
                dl->AddText(nullptr, 0, ImVec2(p.x + S(36), p.y + S(34)), U32(TextDim), text, nullptr, w - S(48));
            }
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, S(4)));
        };
        modeCard(0, ICON_SHIELD, "Nuke prompt (recommended)",
            "Only files saved + verified on the PC can be deleted. Unsaved and KEEP files are blocked.", Accent);
        modeCard(1, ICON_FIRE, "Nuke free",
            "Everything in the Nuke plan is deleted forever, saved or not (internal storage and SD card).", Danger);

        ImGui::Dummy(ImVec2(0, S(8)));
        changed |= ImGui::Checkbox("Auto nuke saved files", &m_settings.autoNukeSaved);
        {
            Font sf(GetFonts().ui, FontSmall);
            ImGui::Indent(ImGui::GetFrameHeight() + S(8));
            ImGui::PushTextWrapPos(0.0f);
            ui::DimText(m_settings.verifyMd5
                    ? "Move mode: each file is deleted from the phone right after it's copied and verified (size + MD5). APK backups are never deleted."
                    : "Move mode: each file is deleted from the phone right after it's copied. Verification is set to size only in Backup; size + MD5 is safer.");
            ImGui::PopTextWrapPos();
            ImGui::Unindent(ImGui::GetFrameHeight() + S(8));
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        changed |= ImGui::Checkbox("Always do a dry run before Nuke", &m_settings.alwaysDryRun);
        ui::ColoredText(Success, ICON_LOCK);
        ImGui::SameLine(0, S(10));
        ImGui::TextUnformatted("Require typing NUKE to confirm");
        ui::ColoredText(Success, ICON_LOCK);
        ImGui::SameLine(0, S(10));
        ImGui::TextUnformatted("Android/data, obb and hidden folders: only when you tick them (Nuke free)");

        ImGui::Dummy(ImVec2(0, S(12)));
        ui::DimText("Background restrictions");
        const size_t here = m_ctx.activeSerial.empty() ? 0 : m_restrictions.ForDevice(m_ctx.DeviceId()).size();
        const size_t total = m_restrictions.Count();
        if (total == 0) {
            ImGui::TextUnformatted("PhoneSweep hasn't restricted any apps.");
        } else {
            ImGui::TextUnformatted(std::format("{} apps restricted on this phone{}", here,
                total > here ? std::format(" ({} more on other phones: connect them to undo)", total - here) : std::string())
                                       .c_str());
            if (ui::SecondaryButton(ICON_ROTATE "  Undo all background restrictions", {}, here > 0 && m_ctx.ActiveConnected()))
                m_ctx.requestUndoAllRestrictions = true;
        }
        {
            Font sf(GetFonts().ui, FontSmall);
            ui::DimText(std::format("Kept in {}", Narrow((Settings::DataDir() / L"restrictions.json").wstring())));
        }
        break;
    }
    case 6: { // Reports
        const auto root = m_settings.ReportsRoot();
        ui::DimText("Reports folder");
        {
            Font mf(GetFonts().mono, FontSmall);
            ImGui::TextWrapped("%s", Narrow(root.wstring()).c_str());
        }
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Change\xE2\x80\xA6##reports")) {
            if (auto p = m_platform.PickFolder(root)) {
                m_settings.reportsRoot = Narrow(p->wstring());
                changed = true;
            }
        }
        if (!m_settings.reportsRoot.empty()) {
            ImGui::SameLine();
            if (ui::GhostButton("Use default##reports")) {
                m_settings.reportsRoot.clear();
                changed = true;
            }
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        changed |= ImGui::Checkbox("Include full permission lists in applist.txt", &m_settings.reportFullPermissions);
        {
            Font sf(GetFonts().ui, FontSmall);
            ImGui::Indent(ImGui::GetFrameHeight() + S(8));
            ui::DimText("Off: only sensitive permissions (SMS, location, accessibility...) are listed.");
            ImGui::Unindent(ImGui::GetFrameHeight() + S(8));
        }
        changed |= ImGui::Checkbox("Mask serial numbers in exports", &m_settings.reportMaskSerial);
        break;
    }
    case 7: // Updates
        changed |= DrawUpdatesTab();
        break;
    case 8: { // About
        ui::Logo(S(72));
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, S(10)));
        {
            Font f(GetFonts().uiStrong, FontH2);
            ImGui::TextUnformatted(kAppNameVersion);
        }
        ui::DimText("Scan, save and clean any Android phone from your PC.");
        ImGui::EndGroup();

        ImGui::Dummy(ImVec2(0, S(10)));
        {
            Font f(GetFonts().ui, FontBase + 1);
            ImGui::TextUnformatted("Made by Kims Ferdy and his humble friend Claude ");
            ImGui::SameLine(0, 0);
            ui::ColoredText(Danger, ICON_HEART);
        }
        ImGui::Dummy(ImVec2(0, S(8)));

        // Links (open in the default browser).
        auto link = [&](const char* icon, const char* label, const char* url) {
            ImGui::PushID(url);
            if (ui::GhostButton(std::format("{}  {}", icon, label).c_str()))
                m_platform.OpenUrl(url);
            ImGui::SetItemTooltip("%s", url);
            ImGui::SameLine(0, S(10));
            {
                Font mf(GetFonts().mono, FontSmall);
                ImGui::AlignTextToFramePadding();
                ui::DimText(url);
            }
            ImGui::PopID();
        };
        ui::DimText("Project");
        link(ICON_CODE_BRANCH, "Source code", kRepoUrl);
        link(ICON_BUG, "Report a bug / ask for a feature", kIssuesUrl);
        link(ICON_DOWNLOAD, "Releases & updates", kReleasesUrl);
        ImGui::Dummy(ImVec2(0, S(6)));
        ui::DimText("Kimscode Studio");
        link(ICON_CAMERA, "Instagram", kInstagramUrl);
        link(ICON_PALETTE, "ArtStation", kArtStationUrl);

        ImGui::Dummy(ImVec2(0, S(10)));
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Open logs folder"))
            m_platform.OpenInExplorer(Settings::DataDir() / L"logs");
        ImGui::SameLine();
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Open settings folder"))
            m_platform.OpenInExplorer(Settings::DataDir());
        break;
    }
    default: {
        static constexpr const char* kWhen[] = { "", "Arrives with saving (Milestone 4).", "Arrives with the file scan (Milestone 1).",
            "Arrives with thumbnails (Milestone 2).", "", "Arrives with Nuke (Milestone 5).", "Arrives with reports (Milestone 6).", "", "" };
        ui::EmptyState(ICON_GEAR, kTabs[m_settingsTab], kWhen[m_settingsTab]);
        break;
    }
    }
    if (changed)
        m_settings.Save();

    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        open = false;
    if (!open) {
        ImGui::CloseCurrentPopup();
        m_ctx.showSettings = false;
    }
    ImGui::EndPopup();
}

// ---- v1.1 updates ----------------------------------------------------------------

void App::PumpUpdater()
{
    if (!m_updater)
        return;
    const Updater::Status s = m_updater->Get();
    const bool newer = s.release && s.release->version > m_updater->Current();
    m_ctx.updateAvailable = newer && s.state != Updater::State::Checking;
    if (s.checkedAt && s.checkedAt != m_seenCheck) {
        m_seenCheck = s.checkedAt;
        if (s.state == Updater::State::UpToDate || s.state == Updater::State::Available) {
            m_settings.lastUpdateCheck = s.checkedAt;
            m_settings.Save();
        }
        if (s.automatic && s.state == Updater::State::Available && !m_updateAnnounced && s.release->version.Str() != m_settings.skippedVersion) {
            m_updateAnnounced = true;
            ui::Toast(std::format("PhoneSweep {} is available: Settings \xE2\x86\x92 Updates", s.release->version.Str()), ui::ToastKind::Info);
        }
    }
    if (newer && s.automatic && s.release->version.Str() == m_settings.skippedVersion)
        m_ctx.updateAvailable = false; // skipped: no dot until the user checks by hand

    // --update-selftest: press the buttons for a real end-to-end run.
    if (m_opts.updateSelfTest) {
        if (s.state == Updater::State::Available)
            m_updater->Download();
        else if (s.state == Updater::State::Ready) {
            std::string err;
            LogInfo("Update self-test: installing {}", Narrow(s.installer.wstring()));
            if (m_updater->Install(&err))
                m_platform.RequestQuit();
            else
                LogError("Update self-test: {}", err);
            m_opts.updateSelfTest = false;
        } else if (s.state == Updater::State::Error || s.state == Updater::State::UpToDate) {
            LogError("Update self-test stopped: {}", s.state == Updater::State::UpToDate ? "up to date" : s.message);
            m_opts.updateSelfTest = false;
        }
    }
}

bool App::DrawUpdatesTab()
{
    bool changed = false;
    const Updater::Status s = m_updater->Get();
    using St = Updater::State;
    const InstallKind kind = m_updater->Kind();

    // What's running now.
    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::TextUnformatted(std::format("PhoneSweep {}", m_updater->Current().Str()).c_str());
    }
    ImGui::SameLine(0, S(10));
    ui::Pill(InstallKindName(kind), TextDim);
    ImGui::Dummy(ImVec2(0, S(4)));

    const bool busy = s.state == St::Checking || s.state == St::Downloading;
    if (ui::SecondaryButton(s.state == St::Checking ? "Checking\xE2\x80\xA6" : ICON_ROTATE "  Check for updates", {}, !busy))
        m_updater->Check(false);
    ImGui::SameLine(0, S(12));
    ImGui::AlignTextToFramePadding();
    const bool newer = s.release && s.release->version > m_updater->Current();
    switch (s.state) {
    case St::Idle: ui::DimText("Not checked yet."); break;
    case St::Checking: ui::DimText("Asking GitHub for the latest release\xE2\x80\xA6"); break;
    case St::UpToDate: ui::ColoredText(Success, ICON_CIRCLE_CHECK "  You're up to date."); break;
    case St::Error:
        if (!newer)
            ui::ColoredText(Warning, std::string(ICON_WARNING "  ") + s.message);
        else
            ui::ColoredText(Accent, "A new version is available.");
        break;
    default: ui::ColoredText(Accent, "A new version is available."); break;
    }
    changed |= ImGui::Checkbox("Check for updates when PhoneSweep starts (once a day)", &m_settings.autoCheckUpdates);
    {
        Font sf(GetFonts().ui, FontSmall);
        ImGui::Indent(ImGui::GetFrameHeight() + S(8));
        ui::DimText("Only asks GitHub which version is the newest. Nothing about you or your phone is sent.");
        ImGui::Unindent(ImGui::GetFrameHeight() + S(8));
    }

    if (!newer)
        return changed;
    const ReleaseInfo& rel = *s.release;
    const ReleaseAsset* asset = PickAsset(rel, kind);

    // The new release.
    ImGui::Dummy(ImVec2(0, S(8)));
    ui::HLine();
    ImGui::Dummy(ImVec2(0, S(6)));
    {
        Font f(GetFonts().uiStrong, FontBase + 2);
        ui::ColoredText(Accent, std::format(ICON_ROCKET "  PhoneSweep {} is available", rel.version.Str()));
    }
    {
        Font sf(GetFonts().ui, FontSmall);
        std::string sub = rel.publishedAt.empty() ? std::string() : "Released " + rel.publishedAt;
        if (asset)
            sub += std::format("{}{} ({})", sub.empty() ? "" : "  \xC2\xB7  ", asset->name, ui::Bytes(asset->size));
        ui::DimText(sub);
    }

    // Release notes (scrolls), then the action row.
    const float actionsH = ImGui::GetFrameHeight() + S(26) + (s.state == St::Downloading || s.state == St::Error ? S(24) : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("##notes", ImVec2(0, std::max(S(80), ImGui::GetContentRegionAvail().y - actionsH)), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        const auto notes = FormatReleaseNotes(rel.notes);
        if (notes.empty())
            ui::DimText("No release notes.");
        ImGui::PushTextWrapPos(0.0f);
        for (const auto& line : notes) {
            switch (line.kind) {
            case NoteLine::Kind::Heading: {
                Font f(GetFonts().uiStrong, FontBase);
                ImGui::TextUnformatted(line.text.c_str());
                break;
            }
            case NoteLine::Kind::Bullet:
                ImGui::Bullet();
                ImGui::TextUnformatted(line.text.c_str());
                break;
            case NoteLine::Kind::Blank: ImGui::Dummy(ImVec2(0, S(4))); break;
            default: ImGui::TextUnformatted(line.text.c_str()); break;
            }
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));

    if (s.state == St::Downloading) {
        const float frac = s.total ? static_cast<float>(s.done) / static_cast<float>(s.total) : 0.0f;
        ui::ProgressBarThin(frac, ImGui::GetContentRegionAvail().x, S(6), Accent);
        ImGui::AlignTextToFramePadding();
        ui::DimText(std::format("Downloading\xE2\x80\xA6 {} of {}", ui::Bytes(s.done), ui::Bytes(s.total)));
        ImGui::SameLine();
        if (ui::GhostButton("Cancel##dl"))
            m_updater->Cancel();
        return changed;
    }
    if (s.state == St::Error)
        ui::ColoredText(Danger, std::string(ICON_WARNING "  ") + s.message);

    if (kind == InstallKind::Portable) {
        if (ui::PrimaryButton(ICON_DOWNLOAD "  Open the download page"))
            m_platform.OpenUrl(rel.pageUrl);
        ImGui::SameLine(0, S(12));
        ImGui::AlignTextToFramePadding();
        ui::DimText("Portable copy: unzip the new version over this folder.");
    } else if (s.state == St::Ready) {
        // Don't pull the rug from under a transfer, scan or delete.
        std::string running;
        for (const auto& j : m_jobs.Visible())
            if (!j->Finished() && j->Name() != "Downloading update" && j->Name() != "Checking for updates")
                running = j->Name();
        if (m_ctx.deleteRunning)
            running = "Deleting files";
        if (ui::PrimaryButton(ICON_ROCKET "  Restart and install", {}, running.empty())) {
            std::string err;
            m_settings.Save();
            if (m_updater->Install(&err))
                m_platform.RequestQuit();
            else
                ui::Toast(err, ui::ToastKind::Error);
        }
        ImGui::SameLine(0, S(12));
        ImGui::AlignTextToFramePadding();
        if (!running.empty())
            ui::ColoredText(Warning, std::format("Waiting for \"{}\" to finish", running));
        else
            ui::DimText(kind == InstallKind::InnoUser ? "PhoneSweep closes, updates itself and opens again."
                                                      : "PhoneSweep closes, Windows asks for permission, then it updates and opens again.");
    } else {
        if (ui::PrimaryButton(ICON_DOWNLOAD "  Download update", {}, asset != nullptr))
            m_updater->Download();
        if (!asset)
            ImGui::SetItemTooltip("This release has no installer of your kind; use the release page.");
        ImGui::SameLine();
        if (ui::SecondaryButton(ICON_EXTERNAL "  Release page"))
            m_platform.OpenUrl(rel.pageUrl);
        ImGui::SameLine();
        const bool skipped = m_settings.skippedVersion == rel.version.Str();
        if (ui::GhostButton(skipped ? "Skipped (undo)" : "Skip this version")) {
            m_settings.skippedVersion = skipped ? std::string() : rel.version.Str();
            changed = true;
        }
        ImGui::SetItemTooltip("Don't announce this version at startup. You can still install it from here.");
    }
    return changed;
}

} // namespace ps
