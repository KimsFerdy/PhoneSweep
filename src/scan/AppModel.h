#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

// Risk + info flags (bitmask). Order = display order of badges.
enum AppFlag : uint32_t {
    AppFlag_None = 0,
    AppFlag_Accessibility = 1u << 0,  // high: can read screen + act as the user
    AppFlag_DeviceAdmin = 1u << 1,    // high: can lock/wipe, resists uninstall
    AppFlag_Sms = 1u << 2,            // high: read/receive/send SMS (2FA codes)
    AppFlag_NotifListener = 1u << 3,  // high: reads every notification
    AppFlag_CallLog = 1u << 4,        // high
    AppFlag_Sideloaded = 1u << 5,     // medium: not from a store
    AppFlag_NoIcon = 1u << 6,         // medium: hidden from the launcher
    AppFlag_Overlay = 1u << 7,        // medium: can draw over other apps
    AppFlag_InstallPackages = 1u << 8, // medium: can install other apps
    AppFlag_BgLocation = 1u << 9,     // medium: location in the background
    AppFlag_Boot = 1u << 10,          // info: starts at boot
    AppFlag_Disabled = 1u << 11,      // info: disabled by the user
};

enum class FlagSeverity { High, Medium, Info };

struct FlagInfo {
    AppFlag flag;
    const char* name;  // stable token used in reports: "SIDELOADED", "SMS"...
    const char* label; // UI badge text
    FlagSeverity severity;
    int weight;        // contribution to the risk score
    const char* why;   // plain-English explanation
};

// All flags in display order.
const std::vector<FlagInfo>& AllFlags();
const FlagInfo* FindFlag(AppFlag f);

struct AppInfo {
    std::string package;
    std::string label;              // "" until read via aapt2
    std::vector<std::string> apkPaths; // base.apk first, then splits
    std::string codePath;           // /data/app/<pkg>-xxxx==
    std::string installer;          // raw installer package ("" = none)
    std::string versionName;
    int64_t versionCode = 0;
    int targetSdk = 0;
    std::string firstInstall;       // "2024-06-08 21:05:47" as dumpsys prints it
    std::string lastUpdate;
    int uid = -1;
    bool system = false;
    bool launcher = false;
    bool enabled = true;
    std::optional<uint64_t> codeBytes, dataBytes, cacheBytes;
    std::vector<std::string> grantedPermissions; // install + runtime (user 0), sorted
    uint32_t flags = 0;
    int risk = 0;                   // sum of flag weights

    uint64_t TotalBytes() const { return codeBytes.value_or(0) + dataBytes.value_or(0) + cacheBytes.value_or(0); }
    bool HasSize() const { return codeBytes || dataBytes; }
    bool Has(AppFlag f) const { return (flags & f) != 0; }
    std::string_view DisplayName() const { return label.empty() ? std::string_view(package) : std::string_view(label); }
};

// "Play Store", "Galaxy Store", "Sideloaded", "Preinstalled", or the raw package.
std::string InstallerName(const AppInfo& app);
bool IsStoreInstaller(std::string_view installer);
// Android "dangerous"/special permissions worth highlighting in the UI.
bool IsSensitivePermission(std::string_view perm);

} // namespace ps
