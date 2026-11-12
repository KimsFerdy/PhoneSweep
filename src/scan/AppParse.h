#pragma once
#include "scan/AppModel.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

// `pm list packages -f -i -U` line:
//   package:/data/app/x-Vm==/base.apk=com.x  installer=com.android.vending uid:10272
struct PmEntry {
    std::string package;
    std::string apkPath;
    std::string installer; // "" when "null"
    int uid = -1;
};
std::vector<PmEntry> ParsePmList(std::string_view text);

// One `Package [x]` block from `dumpsys package packages`.
struct PackageDump {
    std::string codePath;
    std::string versionName;
    int64_t versionCode = 0;
    int targetSdk = 0;
    std::string firstInstall, lastUpdate;
    std::string installer;
    std::vector<std::string> splits;           // "base", "config.arm64_v8a", ...
    std::vector<std::string> grantedInstall;   // install permissions granted=true
    std::vector<std::string> requestedInstall; // all install permissions listed
    std::vector<std::string> grantedRuntime;   // user 0 runtime permissions granted=true
    int user0Enabled = 0; // 0 default, 1 enabled, 2 disabled, 3 disabled-user, 4 disabled-until-used
    bool user0Installed = true;
};
std::map<std::string, PackageDump, std::less<>> ParseDumpsysPackages(std::string_view text);

// `cmd package query-activities/receivers --brief` -> packages with a matching component.
std::set<std::string, std::less<>> ParseComponentPackages(std::string_view text);
// Secure setting "a/.Svc:b/b.Svc" -> {a, b}. "null"/empty -> {}.
std::set<std::string, std::less<>> ParseColonComponents(std::string_view value);
// `dumpsys device_policy` -> packages under "Enabled Device Admins".
std::set<std::string, std::less<>> ParseDeviceAdmins(std::string_view text);

struct DiskSizes {
    uint64_t app = 0, data = 0, cache = 0;
};
// `dumpsys diskstats` "Package Names: [...]" / "App Sizes: [...]" arrays.
std::map<std::string, DiskSizes, std::less<>> ParseDiskStats(std::string_view text);

// `aapt2 dump badging base.apk` -> application label ("" if none).
std::string ParseBadgingLabel(std::string_view text);

// Applies flags + risk score from the gathered facts.
struct RiskContext {
    const std::set<std::string, std::less<>>* launcher = nullptr;
    const std::set<std::string, std::less<>>* boot = nullptr;
    const std::set<std::string, std::less<>>* accessibility = nullptr;
    const std::set<std::string, std::less<>>* notifListeners = nullptr;
    const std::set<std::string, std::less<>>* deviceAdmins = nullptr;
    int deviceSdk = 0;
};
void ComputeFlags(AppInfo& app, const RiskContext& ctx);

} // namespace ps
