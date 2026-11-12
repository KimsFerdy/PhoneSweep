#include "apps/M10SelfTest.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "apps/AppActions.h"
#include "bg/Usage.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <format>

namespace ps {

namespace {
constexpr const char* kPkg = "com.phonesweep.selftest";
}

int RunM10SelfTest(const std::filesystem::path& testApk)
{
    const auto exe = AdbClient::Locate();
    if (!exe) {
        LogError("m10 selftest: adb not found");
        return 2;
    }
    const AdbClient adb(*exe);
    std::string serial;
    for (const auto& d : ParseDevicesList(adb.Run({ "devices", "-l" }).out))
        if (d.state == DeviceEntry::State::Device && serial.empty())
            serial = d.serial;
    if (serial.empty()) {
        LogError("m10 selftest: no device");
        return 3;
    }
    int failures = 0;
    auto check = [&](bool ok, std::string_view what) {
        if (ok)
            LogInfo("m10 selftest: OK   {}", what);
        else {
            LogError("m10 selftest: FAIL {}", what);
            ++failures;
        }
    };
    std::stop_source ss;
    const auto st = ss.get_token();
    using namespace std::chrono_literals;

    // ---- Live CPU/RAM -------------------------------------------------------------
    const auto top = adb.Shell(serial, ShellCommand("top").arg("-b").arg("-n").arg("2").arg("-d").arg("1").arg("-o").arg("PID,USER,%CPU,RES,NAME"), { .timeout = 20s });
    const auto sample = ParseTop(top.out);
    const auto live = GroupTop(sample);
    check(sample.cores > 0 && sample.processes.size() > 20 && !live.empty(),
        std::format("top: {} cores, {} processes, {} apps (top: {} {:.1f}% {})", sample.cores, sample.processes.size(), live.size(),
            live.empty() ? "-" : live[0].package, live.empty() ? 0.0 : live[0].cpu, live.empty() ? "" : FormatBytes(live[0].ramBytes)));

    // ---- Battery --------------------------------------------------------------------
    const auto bs = adb.Shell(serial, ShellCommand("dumpsys").arg("batterystats").arg("--charged"), { .timeout = 90s });
    const auto drain = ParseBatteryStats(bs.out);
    std::vector<AppInfo> apps;
    ForEachLine(adb.Shell(serial, ShellCommand("cmd").arg("package").arg("list").arg("packages").arg("-U")).out, [&](std::string_view l) {
        // package:com.x uid:10123
        const auto t = SplitWhitespace(l);
        if (t.size() == 2 && t[0].starts_with("package:") && t[1].starts_with("uid:")) {
            AppInfo a;
            a.package = std::string(t[0].substr(8));
            a.uid = ParseInt<int>(t[1].substr(4)).value_or(-1);
            apps.push_back(std::move(a));
        }
    });
    UsageStore store;
    store.SetBattery(drain, apps);
    size_t attributed = 0;
    std::string topApp;
    double topMah = 0;
    for (const auto& a : apps)
        if (const auto u = store.Find(a.package); u && u->batteryMah) {
            ++attributed;
            if (*u->batteryMah > topMah) {
                topMah = *u->batteryMah;
                topApp = a.package;
            }
        }
    check(drain.found && drain.capacityMah > 0 && attributed > 5,
        std::format("batterystats: {} rows, capacity {} mAh, computed {:.0f} mAh, {} apps attributed (top {} {:.1f} mAh)", drain.entries.size(),
            drain.capacityMah, drain.computedMah, attributed, topApp, topMah));

    // ---- Uninstall a throwaway app, both ways --------------------------------------
    if (testApk.empty() || !std::filesystem::exists(testApk)) {
        LogWarn("m10 selftest: no test APK given, skipping uninstall checks");
    } else {
        const auto tmp = std::filesystem::temp_directory_path() / L"ps_m10";
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
        const std::string root = "/storage/emulated/0/";
        const std::string mediaDir = root + "Android/media/" + kPkg + "/", dataDir = root + "Android/data/" + kPkg + "/";
        auto installAndSeed = [&]() -> AppInfo {
            const auto in = adb.Run({ "-s", serial, "install", "-r", Narrow(testApk.wstring()) }, { .timeout = 120s });
            check(in.out.find("Success") != std::string::npos, "install throwaway app");
            ShellCommand seed("mkdir");
            seed.arg("-p").arg(mediaDir + "Pictures").arg(dataDir + "files");
            seed.raw("&& echo selftest >").arg(mediaDir + "Pictures/pic.jpg").raw("&& echo selftest >").arg(dataDir + "files/x.bin");
            adb.Shell(serial, seed);
            AppInfo a;
            a.package = kPkg;
            const auto pm = adb.Shell(serial, ShellCommand("pm").arg("path").arg(kPkg)); // keep alive: Trim returns a view
            const auto path = Trim(pm.out);
            if (path.starts_with("package:"))
                a.apkPaths.emplace_back(path.substr(8));
            return a;
        };
        auto installed = [&] { return Trim(adb.Shell(serial, ShellCommand("pm").arg("path").arg(kPkg)).out).starts_with("package:"); };
        auto exists = [&](const std::string& p) { return !Trim(adb.Shell(serial, ShellCommand("ls").arg("-d").arg(p).raw("2>/dev/null")).out).empty(); };

        for (const auto mode : { UninstallMode::Quick, UninstallMode::Full }) {
            const char* name = mode == UninstallMode::Full ? "full" : "quick";
            UninstallTask t;
            t.serial = serial;
            t.app = installAndSeed();
            t.mode = mode;
            t.backupApk = true;
            t.apkDir = tmp / L"apks";
            t.folders = { mediaDir, dataDir };
            t.knownFiles = { mediaDir + "Pictures/pic.jpg", dataDir + "files/x.bin" };
            check(installed() && exists(mediaDir) && exists(dataDir), std::format("{}: app installed + its folders seeded", name));
            UninstallProgress p;
            RunUninstall(adb, t, p, st);
            check(p.ok, std::format("{}: {}", name, p.message));
            check(!installed(), std::format("{}: app is gone", name));
            check(p.apkSaved && std::filesystem::exists(tmp / L"apks" / Widen(kPkg) / L"base.apk"), std::format("{}: APK backed up first", name));
            LogInfo("m10 selftest: {}: after uninstall media={} data={} (reported gone: {})", name, exists(mediaDir), exists(dataDir), p.removedFolders.size());
            if (mode == UninstallMode::Full)
                check(!exists(mediaDir) && !exists(dataDir) && p.removedFolders.size() == 2, "full: Android/media + Android/data folders removed");
            else
                check(p.removedFolders.size() == static_cast<size_t>(!exists(mediaDir)) + static_cast<size_t>(!exists(dataDir)),
                    "quick: reports exactly the folders Android removed");
            std::filesystem::remove_all(tmp, ec);
        }
        // Leave nothing behind.
        adb.Shell(serial, ShellCommand("pm").arg("uninstall").arg(kPkg));
        adb.Shell(serial, ShellCommand("rm").arg("-rf").arg(mediaDir).arg(dataDir));
    }

    LogInfo("m10 selftest: {}", failures == 0 ? "PASS" : std::format("{} FAILURE(S)", failures));
    return failures == 0 ? 0 : 1;
}

} // namespace ps
