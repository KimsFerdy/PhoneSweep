#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ps {

// Persisted to %LOCALAPPDATA%/PhoneSweep/settings.json. Grows with each milestone.
struct Settings {
    // General
    bool autoScan = true;
    bool binaryUnits = false; // GiB instead of GB
    int uiScalePercent = 0;   // 0 = auto (monitor DPI), else 100/125/150

    // Scan
    bool includeSdCard = true;
    int minFileSizeKB = 0; // ignore files smaller than this (0 / 10 / 100)

    // Nuke Protector (user decision: replaces the spec's locked "never delete unverified")
    int nukeMode = 0;           // 0 = Nuke prompt (only saved+verified), 1 = Nuke free
    bool autoNukeSaved = false; // delete from the phone right after a file is saved + verified
    bool alwaysDryRun = true;   // require a dry run of the exact plan before Nuke

    // Reports
    std::string reportsRoot; // "" = <backup root>/reports
    bool reportFullPermissions = true;
    bool reportMaskSerial = true;
    std::filesystem::path ReportsRoot() const;

    // Thumbnails
    bool showThumbnails = true;
    int thumbCacheMB = 1024;
    std::string ffmpegPathOverride; // empty = auto-detect

    // ADB
    std::string adbPathOverride; // empty = auto-detect
    int devicePollMs = 1500;

    // Backup
    std::string lastSaveFolder;
    std::string backupRoot;                       // "" = Documents/PhoneSweep
    std::string folderPattern = "{device}_{date}";
    bool preserveStructure = true;
    bool verifyMd5 = true;
    int parallelTransfers = 2;                    // 1..4
    int conflictPolicy = 0;                       // 0 skip if identical, 1 rename, 2 overwrite
    bool backupApksBeforeUninstall = true;
    bool zipDeleteFolder = false;                 // remove a backup folder once its .zip is verified

    // Wireless (M9)
    std::vector<std::pair<std::string, std::string>> wirelessKnown; // { "192.168.0.3:5555", "Samsung SM-G960F" }
    bool wirelessReconnect = true;                                // reconnect known phones at startup

    std::filesystem::path BackupRoot() const;     // resolved

    static std::filesystem::path DataDir(); // %LOCALAPPDATA%/PhoneSweep
    static Settings Load();
    void Save() const;
};

} // namespace ps
