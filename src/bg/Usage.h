#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class AdbClient;
struct AppInfo;

// ---- Live CPU / RAM (UI spec §8b v0.2) ------------------------------------------

struct TopProcess {
    int pid = 0;
    std::string user;
    double cpu = 0;     // % of ONE core, as top prints it (800% = all 8 cores)
    uint64_t resBytes = 0;
    std::string name;
};
struct TopSample {
    int cores = 0; // from the "800%cpu" header (0 = unknown)
    std::vector<TopProcess> processes;
};
// `top -b -n 1 -o PID,USER,%CPU,RES,NAME`. Pure, unit-tested.
TopSample ParseTop(std::string_view text);
// "4.8M" -> 5033164, "397M", "1.2G", "876K", "0", "123456" (KB, toybox default)
std::optional<uint64_t> ParseTopSize(std::string_view s);

struct LiveApp {
    std::string package;
    double cpu = 0; // summed over the app's processes, % of one core
    uint64_t ramBytes = 0;
    int processes = 0;
    bool system = false; // runs as system/root (not an app user)
};
// App processes (users like u0_a123) grouped by package; heaviest CPU first.
std::vector<LiveApp> GroupTop(const TopSample& sample);
// "Heavy right now": spec says > 15% CPU or > 500 MB RAM.
inline bool IsHeavy(const LiveApp& a) { return a.cpu > 15.0 || a.ramBytes > 500'000'000; }

// ---- Battery (UI spec §8c v0.2) -------------------------------------------------

struct DrainEntry {
    std::string key;    // "u0a274" / "1000" / "Screen"
    int appId = -1;     // 10274 for u0a274 / u95a274 (any user); -1 for hardware rows
    int userId = 0;     // 95 for u95a235 (Secure Folder / work profile)
    double mAh = 0;
};
struct BatteryDrain {
    int capacityMah = 0;
    double computedMah = 0;
    std::vector<DrainEntry> entries;
    bool found = false; // section present
};
// "Estimated power use (mAh):" section of `dumpsys batterystats --charged`.
// Handles "Uid u0a274: 4.46 ( ... )" (Android 9-11) and "UID u0a274: 4.46 fg: ..." (12+).
BatteryDrain ParseBatteryStats(std::string_view text);
// "Screen", "Cell standby", "Idle" ... -> friendlier label; uid 1000 -> "Android System", 0 -> "Kernel (root)".
std::string DrainLabel(const DrainEntry& e);

// ---- Shared per-app usage (Background page fills it, Apps page / Ask AI read it) ----

struct AppUsage {
    std::optional<double> cpu;       // latest live sample, % of one core
    std::optional<uint64_t> ramBytes;
    std::optional<double> batteryMah; // since last full charge (all users of the app)
    std::optional<double> batteryShare; // of the phone's computed drain, 0..1
};

class UsageStore {
public:
    void SetLive(const std::vector<LiveApp>& apps); // replaces live values
    void SetBattery(const BatteryDrain& drain, const std::vector<AppInfo>& apps);
    std::optional<AppUsage> Find(std::string_view package) const;
    void Clear();

private:
    mutable std::mutex m_mutex;
    std::map<std::string, AppUsage, std::less<>> m_usage;
};

} // namespace ps
