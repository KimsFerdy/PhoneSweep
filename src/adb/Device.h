#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class AdbClient;

// One line of `adb devices -l`.
struct DeviceEntry {
    enum class State { Device, Unauthorized, Offline, Other };

    std::string serial;
    State state = State::Other;
    std::string rawState; // e.g. "recovery", "sideload" when State::Other
    std::string model;    // from model:SM_G960F (underscores restored to '-')
    std::string product;
    std::string transportId;

    bool operator==(const DeviceEntry&) const = default;
};

struct StorageStats {
    uint64_t total = 0, used = 0, free = 0; // bytes
    double UsedFraction() const { return total ? static_cast<double>(used) / static_cast<double>(total) : 0.0; }
};

// Everything shown on a device card. Strings are "" when unknown; the UI shows "—".
struct DeviceInfo {
    std::string serial;
    std::string displayName; // marketname, else "Brand Model"
    std::string model;
    std::string brand;
    std::string android;     // "9"
    int sdk = 0;
    std::string oneUi;       // "6.1", "" if not Samsung
    std::string securityPatch;
    std::optional<int> batteryLevel;
    bool charging = false;
    std::optional<double> batteryTempC;
    std::optional<int> ramMarketingGB;
    std::optional<StorageStats> storage;
    bool wireless = false;
    std::string hwSerial; // ro.serialno: the same over USB and Wi-Fi (backups/restrictions are keyed by it)
};

// ---- Pure parsers (unit tested) -------------------------------------------

std::vector<DeviceEntry> ParseDevicesList(std::string_view text);
// `getprop` full dump: "[key]: [value]" lines.
std::map<std::string, std::string, std::less<>> ParseGetprop(std::string_view text);
// One UI from ro.build.version.oneui (60100 -> "6.1"), else from
// ro.build.version.sep (100000 -> "1.0"). Empty if neither present.
std::string DecodeOneUi(std::string_view oneuiProp, std::string_view sepProp);
struct BatteryInfo {
    std::optional<int> level;
    bool charging = false;
    std::optional<double> tempC;
};
BatteryInfo ParseDumpsysBattery(std::string_view text);
std::optional<uint64_t> ParseMemTotalKb(std::string_view meminfo);
int MarketingRamGB(uint64_t memTotalKb);
// `df -k <path>` output; takes the last data row.
std::optional<StorageStats> ParseDfK(std::string_view text);
std::string MaskSerial(std::string_view serial);
// Converts "samsung" -> "Samsung"
std::string CapitalizeBrand(std::string_view brand);

// ---- Live queries ----------------------------------------------------------

// One round trip: getprop + dumpsys battery + meminfo + df.
std::optional<DeviceInfo> QueryDeviceInfo(const AdbClient& adb, std::string_view serial);

} // namespace ps
