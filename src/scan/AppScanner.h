#pragma once
#include "scan/AppModel.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

struct AppScanResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;       // plain English
    std::string errorDetail; // command + stderr
    std::vector<AppInfo> apps; // user apps first, then system apps
    bool launcherKnown = true; // false: NO_ICON couldn't be evaluated on this phone
};

// Gathers every package with metadata and risk flags in ~8 adb calls (no per-app calls).
AppScanResult ScanApps(const AdbClient& adb, std::string_view serial, int deviceSdk, std::stop_token st);

// Finds aapt2: next to the exe, PATH, then the newest Android SDK build-tools.
std::optional<std::filesystem::path> FindAapt2();

// Reads an app's label by pulling base.apk and running `aapt2 dump badging`.
// Returns "" when it can't (no aapt2, pull failed, no label).
std::string ReadAppLabel(const AdbClient& adb, std::string_view serial, const AppInfo& app,
    const std::filesystem::path& aapt2, const std::filesystem::path& tmpDir, std::stop_token st);

// Persistent label cache: %LOCALAPPDATA%/PhoneSweep/labels.json, keyed "pkg@versionCode".
class LabelCache {
public:
    explicit LabelCache(std::filesystem::path file);
    std::optional<std::string> Get(const AppInfo& app) const;
    void Put(const AppInfo& app, const std::string& label); // saves immediately
private:
    static std::string Key(const AppInfo& app);
    std::filesystem::path m_file;
    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_labels;
};

} // namespace ps
