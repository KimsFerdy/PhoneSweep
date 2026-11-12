#include "apps/AppActions.h"

#include "adb/AdbClient.h"
#include "nuke/Nuker.h"
#include "report/AppListFormat.h"
#include "scan/FileStore.h"
#include "transfer/LocalPath.h"
#include "transfer/Manifest.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace ps {

using namespace std::chrono_literals;

std::string PlayStoreUrl(std::string_view package)
{
    const bool valid = !package.empty() && package.size() <= 255 && package.find('.') != std::string_view::npos
        && std::ranges::all_of(package, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_'; });
    return valid ? "https://play.google.com/store/apps/details?id=" + std::string(package) : std::string();
}

std::string FormatAskAi(const AppInfo& app, const std::optional<AppUsage>& usage, std::string_view phone)
{
    std::string out;
    out += std::format("I'm checking an Android app on my phone{}. Please tell me:\n", phone.empty() ? "" : std::format(" ({})", phone));
    out += "1. What is this app and who makes it? Is it the genuine app for this package name?\n";
    out += "2. Is anything about it suspicious (permissions, installer, background behaviour, battery or data use)?\n";
    out += "3. What do the sensitive permissions let it do, and which can I safely revoke?\n";
    out += "4. Is it safe to uninstall, and what would I lose (data, logins, other apps that depend on it)?\n\n";
    out += FormatAppBlock(app);
    if (usage && (usage->cpu || usage->ramBytes || usage->batteryMah)) {
        out += "\nMeasured on the phone:\n";
        if (usage->cpu)
            out += std::format("- CPU right now: {:.1f}% of one core\n", *usage->cpu);
        if (usage->ramBytes)
            out += std::format("- RAM right now: {}\n", FormatBytes(*usage->ramBytes));
        if (usage->batteryMah)
            out += std::format("- Battery since last full charge (Android's estimate): {:.1f} mAh{}\n", *usage->batteryMah,
                usage->batteryShare ? std::format(" ({:.1f}% of all drain)", *usage->batteryShare * 100.0) : std::string());
    }
    return out;
}

std::vector<AppFolder> AppStorageFolders(std::string_view package, const FileStore& files, const VerifiedIndex& verified)
{
    std::vector<AppFolder> out;
    for (const auto& v : files.Volumes())
        for (const char* kind : { "data", "media", "obb" }) {
            AppFolder f;
            f.kind = kind;
            f.path = std::format("{}Android/{}/{}/", v.root, kind, package);
            out.push_back(std::move(f));
        }
    for (const auto& e : files.Files())
        for (auto& f : out)
            if (e.path.starts_with(f.path)) {
                ++f.files;
                f.bytes += e.size;
                if (!verified.IsVerified(e.path, e.size, e.mtime)) {
                    ++f.unsaved;
                    f.unsavedBytes += e.size;
                }
                break;
            }
    std::erase_if(out, [](const AppFolder& f) { return f.files == 0; });
    return out;
}

size_t UnsavedAppFiles(const std::vector<AppFolder>& folders, uint64_t* bytes)
{
    size_t n = 0;
    uint64_t b = 0;
    for (const auto& f : folders)
        if (f.kind != "obb") {
            n += f.unsaved;
            b += f.unsavedBytes;
        }
    if (bytes)
        *bytes = b;
    return n;
}

bool BackupApk(const AdbClient& adb, const std::string& serial, const AppInfo& app, const std::filesystem::path& apkDir, std::stop_token st,
    std::string* error)
{
    if (app.apkPaths.empty()) {
        if (error)
            *error = "no APK path known";
        return false;
    }
    const auto dir = apkDir / Widen(SanitizeComponent(app.package));
    std::error_code ec;
    std::filesystem::create_directories(LongPath(dir), ec);
    for (const auto& apk : app.apkPaths) {
        const auto local = dir / Widen(SanitizeComponent(apk.substr(apk.rfind('/') + 1)));
        const auto r = adb.Pull(serial, apk, local, { .timeout = 10min, .cancel = st });
        const auto stat = adb.Shell(serial, ShellCommand("stat").arg("-c").arg("%s").arg(apk), { .timeout = 15s, .cancel = st });
        const auto remoteSize = ParseInt<uint64_t>(Trim(stat.out));
        const auto localSize = std::filesystem::file_size(LongPath(local), ec);
        if (!r.ok() || !remoteSize || ec || localSize != *remoteSize) {
            if (error)
                *error = "couldn't copy " + apk.substr(apk.rfind('/') + 1) + (r.err.empty() ? std::string() : ": " + std::string(Trim(r.err)));
            LogError("APK backup failed for {} ({}): {}", app.package, apk, r.err);
            return false;
        }
    }
    LogInfo("APK backed up: {} -> {}", app.package, Narrow(dir.wstring()));
    return true;
}

void RunUninstall(const AdbClient& adb, const UninstallTask& t, UninstallProgress& p, std::stop_token st)
{
    auto step = [&](std::string s) {
        std::lock_guard l(p.mutex);
        p.step = std::move(s);
    };
    auto finish = [&](bool ok, std::string msg) {
        if (ok)
            LogWarn("{} uninstall of {}: {}", t.mode == UninstallMode::Full ? "Full" : "Quick", t.app.package, msg);
        else
            LogError("{} uninstall of {}: {}", t.mode == UninstallMode::Full ? "Full" : "Quick", t.app.package, msg);
        std::lock_guard l(p.mutex);
        p.ok = ok;
        p.message = std::move(msg);
        p.finished = true;
    };
    const std::string& pkg = t.app.package;

    // 1. APK first: no backup, no uninstall.
    if (t.backupApk) {
        step("Backing up the APK");
        std::string err;
        if (!BackupApk(adb, t.serial, t.app, t.apkDir, st, &err)) {
            finish(false, "APK backup failed (" + err + "), so nothing was uninstalled");
            return;
        }
        std::lock_guard l(p.mutex);
        p.apkSaved = true;
    }
    if (st.stop_requested()) {
        finish(false, "Cancelled; nothing was uninstalled");
        return;
    }

    // 2. Full: wipe data + cache first (what "Clear storage" in Android settings does).
    if (t.mode == UninstallMode::Full) {
        step("Clearing data and cache");
        const auto c = adb.Shell(t.serial, ShellCommand("pm").arg("clear").arg(pkg), { .timeout = 60s, .cancel = st });
        std::lock_guard l(p.mutex);
        p.dataCleared = c.out.find("Success") != std::string::npos;
        if (!p.dataCleared)
            LogWarn("pm clear {}: {}", pkg, Trim(c.out + c.err));
    }

    // 3. Uninstall.
    step("Uninstalling");
    const auto r = adb.Shell(t.serial, ShellCommand("pm").arg("uninstall").arg(pkg), { .timeout = 120s, .cancel = st });
    if (r.out.find("Success") == std::string::npos) {
        std::string reason(Trim(r.out.empty() ? r.err : r.out));
        if (reason.find("DEVICE_POLICY") != std::string::npos)
            reason = "it's a device admin: turn it off under Settings > Security > Device admin apps (the exact menu name varies by phone), then try again";
        finish(false, "Not uninstalled: " + reason);
        return;
    }

    // 4. The app's folders on shared storage. Full: delete leftovers. Both: report which
    // are gone (Android removes Android/data and obb itself) so the file list can follow.
    size_t removed = 0;
    if (!t.folders.empty()) {
        step(t.mode == UninstallMode::Full ? "Removing leftover folders" : "Checking the app's folders");
        std::vector<std::string> goneFiles;
        for (const auto& f : t.folders) {
            // Defence in depth: only ever .../Android/{data,media,obb}/<this package>/
            const bool shaped = f.ends_with("/" + pkg + "/") && f.starts_with("/storage/")
                && (f.find("/Android/data/") != std::string::npos || f.find("/Android/media/") != std::string::npos
                    || f.find("/Android/obb/") != std::string::npos);
            if (!shaped) {
                LogError("Refusing to touch unexpected folder {}", f);
                continue;
            }
            if (t.mode == UninstallMode::Full)
                adb.Shell(t.serial, ShellCommand("rm").arg("-rf").arg(f), { .timeout = 120s, .cancel = st });
            const auto left = adb.Shell(t.serial, ShellCommand("ls").arg("-d").arg(f).raw("2>/dev/null"), { .timeout = 15s, .cancel = st });
            if (Trim(left.out).empty()) {
                ++removed;
                for (const auto& file : t.knownFiles)
                    if (file.starts_with(f))
                        goneFiles.push_back(file);
                std::lock_guard l(p.mutex);
                p.removedFolders.push_back(f);
            }
        }
        if (!goneFiles.empty())
            CleanMediaStore(adb, t.serial, goneFiles, st); // no gallery ghosts
    }
    std::string msg = "Uninstalled";
    if (t.mode == UninstallMode::Full) {
        std::lock_guard l(p.mutex);
        msg += p.dataCleared ? ", data and cache cleared" : "";
    }
    if (removed)
        msg += std::format(", {} app folder{} gone from storage", removed, removed == 1 ? "" : "s");
    if (t.backupApk)
        msg += " (APK saved on the PC)";
    finish(true, msg);
}

} // namespace ps
