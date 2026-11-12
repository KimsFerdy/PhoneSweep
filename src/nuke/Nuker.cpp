#include "nuke/Nuker.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "nuke/NukePlan.h"
#include "transfer/LocalPath.h"
#include "transfer/Manifest.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <fstream>
#include <set>

namespace ps {

namespace {

constexpr size_t kDeleteBatch = 500;
constexpr size_t kRescanBatch = 100;
constexpr const char* kRemoteList = "/data/local/tmp/ps_delete.txt";

// Writes paths NUL-separated (exact bytes: newlines, quotes, invalid UTF-8 all survive),
// pushes the file to the phone. Returns false if the push failed.
bool PushList(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths, const std::filesystem::path& tmpDir,
    std::stop_token st)
{
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const auto local = tmpDir / L"ps_list.bin";
    {
        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        for (const auto& p : paths) {
            out.write(p.data(), static_cast<std::streamsize>(p.size()));
            out.put('\0');
        }
        if (!out)
            return false;
    }
    const auto r = adb.Push(serial, local, kRemoteList, { .timeout = 60s, .cancel = st });
    std::filesystem::remove(local, ec);
    return r.ok();
}

// Paths from the pushed list that still exist (newline-separated output).
std::vector<std::string> Survivors(const AdbClient& adb, const std::string& serial, std::stop_token st)
{
    ShellCommand cmd("xargs");
    cmd.arg("-0").arg("ls").arg("-d").arg("--").raw(std::string("< ") + kRemoteList + " 2>/dev/null");
    const auto r = adb.Shell(serial, cmd, { .timeout = 120s, .cancel = st });
    std::vector<std::string> out;
    ForEachLine(r.out, [&](std::string_view l) {
        if (!l.empty())
            out.emplace_back(l);
    });
    return out;
}

std::vector<VolumeSpace> Space(const AdbClient& adb, const NukeTask& t, std::stop_token st)
{
    std::vector<VolumeSpace> out;
    for (size_t i = 0; i < t.volumeRoots.size(); ++i) {
        const auto r = adb.Shell(t.serial, ShellCommand("df").arg("-k").arg(t.volumeRoots[i]), { .timeout = 15s, .cancel = st });
        VolumeSpace v;
        v.label = i < t.volumeLabels.size() ? t.volumeLabels[i] : t.volumeRoots[i];
        if (auto s = ParseDfK(r.out)) {
            v.total = s->total;
            v.used = s->used;
            v.free = s->free;
        }
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace

const char* PhaseName(NukePhase p)
{
    switch (p) {
    case NukePhase::Starting: return "Starting";
    case NukePhase::BackupApks: return "Backing up APKs";
    case NukePhase::DeleteFiles: return "Deleting files";
    case NukePhase::RemoveFolders: return "Removing empty folders";
    case NukePhase::Uninstall: return "Uninstalling apps";
    case NukePhase::ClearCaches: return "Clearing caches";
    case NukePhase::MediaRescan: return "Updating the gallery";
    case NukePhase::Done: return "Done";
    }
    return "?";
}

void CleanMediaStore(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& deletedPaths, std::stop_token st)
{
    std::vector<std::string> valid;
    for (const auto& d : deletedPaths)
        if (IsValidUtf8(d))
            valid.push_back(d);
    for (size_t i = 0; i < valid.size() && !st.stop_requested(); i += kRescanBatch) {
        std::string where = "_data IN (";
        for (size_t k = i; k < std::min(valid.size(), i + kRescanBatch); ++k) {
            if (k > i)
                where += ",";
            where += SqlQuote(valid[k]);
        }
        where += ")";
        ShellCommand cmd("content");
        cmd.arg("delete").arg("--uri").arg("content://media/external/file").arg("--where").arg(where);
        adb.Shell(serial, cmd, { .timeout = 60s, .cancel = st });
    }
}

std::vector<std::string> DeleteFiles(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths,
    const std::filesystem::path& tmpDir, std::stop_token st)
{
    std::vector<std::string> gone;
    for (size_t i = 0; i < paths.size() && !st.stop_requested(); i += kDeleteBatch) {
        const std::vector<std::string> batch(paths.begin() + i, paths.begin() + std::min(paths.size(), i + kDeleteBatch));
        if (!PushList(adb, serial, batch, tmpDir, st))
            break;
        ShellCommand rm("xargs");
        rm.arg("-0").arg("rm").arg("-f").arg("--").raw(std::string("< ") + kRemoteList);
        adb.Shell(serial, rm, { .timeout = 10min, .cancel = st });
        const auto left = Survivors(adb, serial, st);
        const std::set<std::string> leftSet(left.begin(), left.end());
        for (const auto& p : batch)
            if (!leftSet.contains(p))
                gone.push_back(p);
        for (const auto& p : batch)
            LogInfo("{} {}", leftSet.contains(p) ? "KEPT (delete failed)" : "deleted", p);
    }
    adb.Shell(serial, ShellCommand("rm").arg("-f").arg(kRemoteList), { .timeout = 15s });
    return gone;
}

void RunNuke(const AdbClient& adb, const NukeTask& t, NukeProgress& p, std::stop_token st)
{
    auto phase = [&](NukePhase ph, size_t total) {
        std::lock_guard l(p.mutex);
        p.phase = ph;
        p.phaseDone = 0;
        p.phaseTotal = total;
        LogInfo("NUKE phase: {} ({} items)", PhaseName(ph), total);
    };
    auto step = [&](size_t n = 1) {
        std::lock_guard l(p.mutex);
        p.phaseDone += n;
    };
    auto stopped = [&] {
        if (!st.stop_requested())
            return false;
        std::lock_guard l(p.mutex);
        p.cancelled = true;
        p.finished = true;
        LogWarn("NUKE cancelled during {}", PhaseName(p.phase));
        return true;
    };

    LogWarn("NUKE started: {} files, {} apps, caches={}", t.deletePaths.size(), t.apps.size(), t.clearCaches);
    {
        auto before = Space(adb, t, st);
        std::lock_guard l(p.mutex);
        p.before = std::move(before);
    }

    // 1. APK backups for apps about to be uninstalled. No backup -> no uninstall.
    std::set<std::string> backupFailed;
    if (!t.apps.empty() && t.backupApks) {
        phase(NukePhase::BackupApks, t.apps.size());
        for (const auto& app : t.apps) {
            if (stopped())
                return;
            bool ok = !app.apkPaths.empty();
            for (const auto& apk : app.apkPaths) {
                const auto dir = t.apkBackupDir / Widen(SanitizeComponent(app.package));
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const auto local = dir / Widen(SanitizeComponent(apk.substr(apk.rfind('/') + 1)));
                const auto r = adb.Pull(t.serial, apk, local, { .timeout = 10min, .cancel = st });
                const auto stat = adb.Shell(t.serial, ShellCommand("stat").arg("-c").arg("%s").arg(apk), { .timeout = 15s, .cancel = st });
                const auto remoteSize = ParseInt<uint64_t>(Trim(stat.out));
                const auto localSize = std::filesystem::file_size(local, ec);
                if (!r.ok() || !remoteSize || ec || localSize != *remoteSize) {
                    ok = false;
                    LogError("APK backup failed for {} ({}): {}", app.package, apk, r.err);
                }
            }
            if (ok) {
                std::lock_guard l(p.mutex);
                ++p.apksBackedUp;
            } else {
                backupFailed.insert(app.package);
            }
            step();
        }
    }

    // 2. Delete files in batches (NUL-separated lists: any file name is safe).
    if (!t.deletePaths.empty()) {
        phase(NukePhase::DeleteFiles, t.deletePaths.size());
        for (size_t i = 0; i < t.deletePaths.size(); i += kDeleteBatch) {
            if (stopped())
                return;
            const std::vector<std::string> batch(t.deletePaths.begin() + i, t.deletePaths.begin() + std::min(t.deletePaths.size(), i + kDeleteBatch));
            const auto gone = DeleteFiles(adb, t.serial, batch, t.tmpDir, st);
            std::lock_guard l(p.mutex);
            p.deleted += gone.size();
            p.deleteFailed += batch.size() - gone.size();
            p.deletedPaths.insert(p.deletedPaths.end(), gone.begin(), gone.end());
            if (gone.size() < batch.size() && p.failedSamples.size() < 20) {
                const std::set<std::string> goneSet(gone.begin(), gone.end());
                for (const auto& path : batch)
                    if (!goneSet.contains(path) && p.failedSamples.size() < 20)
                        p.failedSamples.push_back(path);
            }
            p.phaseDone += batch.size();
        }
    }

    // 3. Folders left empty (rmdir refuses non-empty ones, so this is safe by construction).
    std::vector<std::string> deleted;
    {
        std::lock_guard l(p.mutex);
        deleted = p.deletedPaths;
    }
    if (t.removeEmptyFolders && !deleted.empty()) {
        const auto dirs = CandidateEmptyDirs(deleted, t.volumeRoots);
        phase(NukePhase::RemoveFolders, dirs.size());
        if (!dirs.empty() && !stopped() && PushList(adb, t.serial, dirs, t.tmpDir, st)) {
            ShellCommand rd("xargs");
            rd.arg("-0").arg("rmdir").raw(std::string("< ") + kRemoteList + " 2>/dev/null");
            adb.Shell(t.serial, rd, { .timeout = 5min, .cancel = st });
            const auto left = Survivors(adb, t.serial, st);
            adb.Shell(t.serial, ShellCommand("rm").arg("-f").arg(kRemoteList), { .timeout = 15s });
            std::lock_guard l(p.mutex);
            p.foldersRemoved = dirs.size() - std::min(dirs.size(), left.size());
            p.phaseDone = dirs.size();
        }
    }

    // 4. Uninstall flagged apps (skipping any whose APK backup failed).
    if (!t.apps.empty()) {
        phase(NukePhase::Uninstall, t.apps.size());
        for (const auto& app : t.apps) {
            if (stopped())
                return;
            std::string reason;
            if (backupFailed.contains(app.package)) {
                reason = "APK backup failed, so it was not uninstalled";
            } else {
                const auto r = adb.Shell(t.serial, ShellCommand("pm").arg("uninstall").arg(app.package), { .timeout = 120s, .cancel = st });
                if (r.out.find("Success") == std::string::npos) {
                    reason = std::string(Trim(r.out.empty() ? r.err : r.out));
                    if (reason.find("DEVICE_POLICY") != std::string::npos)
                        reason = "It's a device admin: turn it off under Settings > Security > Device admin apps (the exact menu name varies by phone), then try again";
                }
            }
            std::lock_guard l(p.mutex);
            if (reason.empty()) {
                p.uninstalled.push_back(app.package);
                LogWarn("Uninstalled {}", app.package);
            } else {
                p.uninstallFailed.push_back({ app.package, reason });
                LogError("Uninstall failed for {}: {}", app.package, reason);
            }
            ++p.phaseDone;
        }
    }

    // 5. Caches.
    if (t.clearCaches && !stopped()) {
        phase(NukePhase::ClearCaches, 1);
        const auto r = adb.Shell(t.serial, ShellCommand("pm").arg("trim-caches").arg("999G"), { .timeout = 120s, .cancel = st });
        std::lock_guard l(p.mutex);
        p.cachesCleared = r.ok();
        p.phaseDone = 1;
    }

    // 6. Gallery: drop MediaStore rows of deleted files so they don't show as broken thumbnails.
    if (t.mediaRescan && !deleted.empty() && !stopped()) {
        std::vector<std::string> valid;
        for (const auto& d : deleted)
            if (IsValidUtf8(d))
                valid.push_back(d);
        phase(NukePhase::MediaRescan, valid.size());
        for (size_t i = 0; i < valid.size(); i += kRescanBatch) {
            if (stopped())
                return;
            std::string where = "_data IN (";
            for (size_t k = i; k < std::min(valid.size(), i + kRescanBatch); ++k) {
                if (k > i)
                    where += ",";
                where += SqlQuote(valid[k]);
            }
            where += ")";
            ShellCommand cmd("content");
            cmd.arg("delete").arg("--uri").arg("content://media/external/file").arg("--where").arg(where);
            adb.Shell(t.serial, cmd, { .timeout = 60s, .cancel = st });
            step(std::min(kRescanBatch, valid.size() - i));
        }
    }

    auto after = Space(adb, t, st);
    std::lock_guard l(p.mutex);
    p.after = std::move(after);
    p.phase = NukePhase::Done;
    p.finished = true;
    LogWarn("NUKE finished: {} deleted, {} failed, {} folders removed, {} apps uninstalled, {} uninstall failures", p.deleted, p.deleteFailed,
        p.foldersRemoved, p.uninstalled.size(), p.uninstallFailed.size());
}

} // namespace ps
