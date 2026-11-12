#include "scan/AppScanner.h"

#include "adb/AdbClient.h"
#include "scan/AppParse.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>

namespace ps {

namespace {

constexpr const char* kSep = "@@PS@@";

std::pair<std::string_view, std::string_view> SplitOnce(std::string_view s, std::string_view sep)
{
    const auto p = s.find(sep);
    if (p == std::string_view::npos)
        return { s, {} };
    return { s.substr(0, p), s.substr(p + sep.size()) };
}

} // namespace

AppScanResult ScanApps(const AdbClient& adb, std::string_view serial, int deviceSdk, std::stop_token st)
{
    AppScanResult res;
    const RunOptions opt{ .timeout = 60s, .cancel = st };
    auto cancelled = [&] {
        res.cancelled = st.stop_requested();
        return res.cancelled;
    };

    // 1-2. Package lists (user + system), with APK path, installer and uid.
    const auto userList = adb.Shell(serial, ShellCommand("pm").arg("list").arg("packages").arg("-3").arg("-f").arg("-i").arg("-U"), opt);
    if (cancelled())
        return res;
    if (!userList.ok()) {
        res.error = "Couldn't list the phone's apps.";
        res.errorDetail = "pm list packages -3 -f -i -U\n" + userList.err;
        return res;
    }
    const auto sysList = adb.Shell(serial, ShellCommand("pm").arg("list").arg("packages").arg("-s").arg("-f").arg("-i").arg("-U"), opt);

    // 3. Everything else about every package in one dump.
    const auto dump = adb.Shell(serial, ShellCommand("dumpsys").arg("package").arg("packages"), { .timeout = 120s, .cancel = st });
    if (cancelled())
        return res;
    const auto dumps = ParseDumpsysPackages(dump.out);

    // 4. Launcher activities (NO_ICON).
    std::optional<std::set<std::string, std::less<>>> launcher;
    {
        ShellCommand cmd("cmd");
        cmd.arg("package").arg("query-activities").arg("--brief").arg("-a").arg("android.intent.action.MAIN").arg("-c").arg(
            "android.intent.category.LAUNCHER");
        const auto r = adb.Shell(serial, cmd, opt);
        if (r.ok() && r.out.find("activities found") != std::string::npos)
            launcher = ParseComponentPackages(r.out);
        else
            LogWarn("Launcher query unsupported on this phone; NO_ICON can't be checked");
    }
    res.launcherKnown = launcher.has_value();

    // 5. Boot receivers (BOOT_COMPLETED + LOCKED_BOOT_COMPLETED).
    std::set<std::string, std::less<>> boot;
    for (const char* action : { "android.intent.action.BOOT_COMPLETED", "android.intent.action.LOCKED_BOOT_COMPLETED" }) {
        const auto r = adb.Shell(serial, ShellCommand("cmd").arg("package").arg("query-receivers").arg("--brief").arg("-a").arg(action), opt);
        if (r.ok())
            boot.merge(ParseComponentPackages(r.out));
    }

    // 6. Accessibility services + notification listeners (one call).
    std::set<std::string, std::less<>> access, notif;
    {
        ShellCommand cmd("settings");
        cmd.arg("get").arg("secure").arg("enabled_accessibility_services").raw("; echo").arg(kSep).raw(";");
        cmd.then("", ShellCommand("settings").arg("get").arg("secure").arg("enabled_notification_listeners"));
        const auto r = adb.Shell(serial, cmd, opt);
        const auto [a, n] = SplitOnce(r.out, kSep);
        access = ParseColonComponents(a);
        notif = ParseColonComponents(n);
    }

    // 7. Device admins.
    const auto admins = ParseDeviceAdmins(adb.Shell(serial, ShellCommand("dumpsys").arg("device_policy"), opt).out);

    // 8. Sizes.
    const auto sizes = ParseDiskStats(adb.Shell(serial, ShellCommand("dumpsys").arg("diskstats"), opt).out);
    if (cancelled())
        return res;

    RiskContext ctx;
    ctx.launcher = launcher ? &*launcher : nullptr;
    ctx.boot = &boot;
    ctx.accessibility = &access;
    ctx.notifListeners = &notif;
    ctx.deviceAdmins = &admins;
    ctx.deviceSdk = deviceSdk;

    auto build = [&](const PmEntry& pm, bool system) {
        AppInfo a;
        a.package = pm.package;
        a.uid = pm.uid;
        a.installer = pm.installer;
        a.system = system;
        if (!pm.apkPath.empty())
            a.apkPaths.push_back(pm.apkPath);
        if (auto it = dumps.find(pm.package); it != dumps.end()) {
            const PackageDump& d = it->second;
            a.codePath = d.codePath;
            a.versionName = d.versionName;
            a.versionCode = d.versionCode;
            a.targetSdk = d.targetSdk;
            a.firstInstall = d.firstInstall;
            a.lastUpdate = d.lastUpdate;
            if (a.installer.empty())
                a.installer = d.installer;
            a.enabled = d.user0Enabled == 0 || d.user0Enabled == 1;
            for (const auto& s : d.splits)
                if (s != "base" && !d.codePath.empty())
                    a.apkPaths.push_back(d.codePath + "/split_" + s + ".apk");
            a.grantedPermissions = d.grantedInstall;
            a.grantedPermissions.insert(a.grantedPermissions.end(), d.grantedRuntime.begin(), d.grantedRuntime.end());
            std::ranges::sort(a.grantedPermissions);
            a.grantedPermissions.erase(std::unique(a.grantedPermissions.begin(), a.grantedPermissions.end()), a.grantedPermissions.end());
        }
        if (auto it = sizes.find(pm.package); it != sizes.end()) {
            a.codeBytes = it->second.app;
            a.dataBytes = it->second.data;
            a.cacheBytes = it->second.cache;
        }
        a.launcher = launcher && launcher->contains(a.package);
        ComputeFlags(a, ctx);
        return a;
    };

    for (const auto& pm : ParsePmList(userList.out))
        res.apps.push_back(build(pm, false));
    for (const auto& pm : ParsePmList(sysList.out))
        res.apps.push_back(build(pm, true));

    const auto flagged = std::ranges::count_if(res.apps, [](const AppInfo& a) { return !a.system && a.risk > 0; });
    LogInfo("App scan: {} user apps, {} system apps, {} user apps with risk flags", std::ranges::count_if(res.apps, [](auto& a) { return !a.system; }),
        std::ranges::count_if(res.apps, [](auto& a) { return a.system; }), flagged);
    res.ok = true;
    return res;
}

std::optional<std::filesystem::path> FindAapt2()
{
    if (auto p = FindExecutable(L"aapt2.exe"))
        return p;
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0)
        return std::nullopt;
    const auto bt = std::filesystem::path(local) / L"Android" / L"Sdk" / L"build-tools";
    std::error_code ec;
    std::optional<std::filesystem::path> best;
    for (std::filesystem::directory_iterator it(bt, ec), end; !ec && it != end; it.increment(ec)) {
        const auto cand = it->path() / L"aapt2.exe";
        if (std::filesystem::is_regular_file(cand, ec) && (!best || cand > *best)) // lexical max ~ newest version
            best = cand;
    }
    return best;
}

std::string ReadAppLabel(const AdbClient& adb, std::string_view serial, const AppInfo& app, const std::filesystem::path& aapt2,
    const std::filesystem::path& tmpDir, std::stop_token st)
{
    if (app.apkPaths.empty())
        return {};
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const auto local = tmpDir / Widen(app.package + ".apk");
    struct Guard {
        std::filesystem::path p;
        ~Guard()
        {
            std::error_code e;
            std::filesystem::remove(p, e);
        }
    } guard{ local };

    const auto pull = adb.Pull(serial, app.apkPaths.front(), local, { .timeout = 10min, .cancel = st });
    if (!pull.ok())
        return {};
    const std::vector<std::string> args = { "dump", "badging", Narrow(local.wstring()) };
    const auto r = RunProcess(aapt2, args, { .timeout = 60s, .cancel = st, .logName = "aapt2" });
    if (!r.ok() && r.out.empty())
        return {};
    return ParseBadgingLabel(r.out);
}

LabelCache::LabelCache(std::filesystem::path file)
    : m_file(std::move(file))
{
    std::ifstream in(m_file, std::ios::binary);
    if (!in)
        return;
    try {
        const auto j = nlohmann::json::parse(in);
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_string())
                m_labels[it.key()] = it.value().get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        LogWarn("labels.json unreadable: {}", e.what());
    }
}

std::string LabelCache::Key(const AppInfo& app) { return std::format("{}@{}", app.package, app.versionCode); }

std::optional<std::string> LabelCache::Get(const AppInfo& app) const
{
    std::lock_guard l(m_mutex);
    if (auto it = m_labels.find(Key(app)); it != m_labels.end())
        return it->second;
    return std::nullopt;
}

void LabelCache::Put(const AppInfo& app, const std::string& label)
{
    std::lock_guard l(m_mutex);
    m_labels[Key(app)] = label;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : m_labels)
        j[k] = v;
    auto tmp = m_file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, m_file, ec);
}

} // namespace ps
