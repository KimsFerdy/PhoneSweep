#include "app/Settings.h"

#include "transfer/LocalPath.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace ps {

namespace {

template <class T>
void Get(const nlohmann::json& j, const char* key, T& out)
{
    if (auto it = j.find(key); it != j.end()) {
        try {
            out = it->get<T>();
        } catch (const nlohmann::json::exception&) {
            // Keep the default for a malformed value rather than failing the whole file.
        }
    }
}

} // namespace

std::filesystem::path Settings::DataDir()
{
    wchar_t* local = nullptr;
    size_t len = 0;
    std::filesystem::path base;
    if (_wdupenv_s(&local, &len, L"LOCALAPPDATA") == 0 && local) {
        base = local;
        free(local);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    auto dir = base / L"PhoneSweep";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path Settings::BackupRoot() const
{
    if (!backupRoot.empty())
        return std::filesystem::path(Widen(backupRoot));
    return DocumentsFolder() / L"PhoneSweep";
}

std::filesystem::path Settings::ReportsRoot() const
{
    if (!reportsRoot.empty())
        return std::filesystem::path(Widen(reportsRoot));
    return BackupRoot() / L"reports";
}

Settings Settings::Load()
{
    Settings s;
    std::ifstream in(DataDir() / L"settings.json", std::ios::binary);
    if (!in)
        return s;
    try {
        const auto j = nlohmann::json::parse(in);
        Get(j, "autoScan", s.autoScan);
        Get(j, "binaryUnits", s.binaryUnits);
        Get(j, "uiScalePercent", s.uiScalePercent);
        Get(j, "includeSdCard", s.includeSdCard);
        Get(j, "minFileSizeKB", s.minFileSizeKB);
        Get(j, "backupRoot", s.backupRoot);
        Get(j, "folderPattern", s.folderPattern);
        Get(j, "preserveStructure", s.preserveStructure);
        Get(j, "verifyMd5", s.verifyMd5);
        Get(j, "parallelTransfers", s.parallelTransfers);
        Get(j, "conflictPolicy", s.conflictPolicy);
        Get(j, "backupApksBeforeUninstall", s.backupApksBeforeUninstall);
        Get(j, "reportsRoot", s.reportsRoot);
        Get(j, "reportFullPermissions", s.reportFullPermissions);
        Get(j, "reportMaskSerial", s.reportMaskSerial);
        Get(j, "nukeMode", s.nukeMode);
        Get(j, "autoNukeSaved", s.autoNukeSaved);
        Get(j, "alwaysDryRun", s.alwaysDryRun);
        Get(j, "showThumbnails", s.showThumbnails);
        Get(j, "thumbCacheMB", s.thumbCacheMB);
        Get(j, "ffmpegPathOverride", s.ffmpegPathOverride);
        Get(j, "adbPathOverride", s.adbPathOverride);
        Get(j, "devicePollMs", s.devicePollMs);
        Get(j, "lastSaveFolder", s.lastSaveFolder);
        Get(j, "wirelessKnown", s.wirelessKnown);
        Get(j, "wirelessReconnect", s.wirelessReconnect);
        Get(j, "zipDeleteFolder", s.zipDeleteFolder);
    } catch (const nlohmann::json::exception& e) {
        LogWarn("settings.json unreadable, using defaults: {}", e.what());
    }
    return s;
}

void Settings::Save() const
{
    const nlohmann::json j = {
        { "autoScan", autoScan },
        { "binaryUnits", binaryUnits },
        { "uiScalePercent", uiScalePercent },
        { "includeSdCard", includeSdCard },
        { "minFileSizeKB", minFileSizeKB },
        { "backupRoot", backupRoot },
        { "folderPattern", folderPattern },
        { "preserveStructure", preserveStructure },
        { "verifyMd5", verifyMd5 },
        { "parallelTransfers", parallelTransfers },
        { "conflictPolicy", conflictPolicy },
        { "backupApksBeforeUninstall", backupApksBeforeUninstall },
        { "reportsRoot", reportsRoot },
        { "reportFullPermissions", reportFullPermissions },
        { "reportMaskSerial", reportMaskSerial },
        { "nukeMode", nukeMode },
        { "autoNukeSaved", autoNukeSaved },
        { "alwaysDryRun", alwaysDryRun },
        { "showThumbnails", showThumbnails },
        { "thumbCacheMB", thumbCacheMB },
        { "ffmpegPathOverride", ffmpegPathOverride },
        { "adbPathOverride", adbPathOverride },
        { "devicePollMs", devicePollMs },
        { "lastSaveFolder", lastSaveFolder },
        { "wirelessKnown", wirelessKnown },
        { "wirelessReconnect", wirelessReconnect },
        { "zipDeleteFolder", zipDeleteFolder },
    };
    const auto path = DataDir() / L"settings.json";
    const auto tmp = DataDir() / L"settings.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2);
        if (!out)
            return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec); // atomic replace on NTFS
}

} // namespace ps
