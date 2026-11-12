#pragma once
#include "transfer/Manifest.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

struct BackupFolderInfo {
    std::filesystem::path folder; // a folder, or a .zip (M9)
    std::string serial, model;
    size_t files = 0;
    uint64_t bytes = 0;
    bool zip = false;
};
// Every backup folder and backup .zip (any phone) under the backup root, newest name first.
std::vector<BackupFolderInfo> ListBackupFolders(const std::filesystem::path& root);

// Where a backed-up file goes on THIS phone. Internal paths stay as they are;
// SD card paths move to this phone's SD card, or to "Restored from SD card/" on
// internal storage when there is none. Pure, unit-tested.
std::string MapRestoreTarget(std::string_view devicePath, const std::vector<std::string>& volumeRoots);

// "/storage/emulated/0/" plus "/storage/XXXX-XXXX/" for each mounted SD card / USB drive.
std::vector<std::string> PhoneVolumeRoots(const AdbClient& adb, const std::string& serial, std::stop_token st);

enum class RestoreState : uint8_t {
    Missing,    // not on the phone: restore it
    Present,    // same size already on the phone
    Different,  // a different file has that name
    NoLocal,    // the backup copy is gone from the PC
};
const char* ToString(RestoreState s);

struct RestoreItem {
    ManifestEntry entry;
    std::filesystem::path local; // the file on the PC, or the .zip holding it
    std::string zipEntry;        // name inside `local` when it's a zip
    std::string target;          // where it goes on the phone
    RestoreState state = RestoreState::Missing;
};

// Reads the folder's manifest and checks each target on the phone (batched).
std::vector<RestoreItem> PlanRestore(const AdbClient& adb, const std::string& serial, const std::filesystem::path& folder,
    const std::vector<std::string>& volumeRoots, std::stop_token st);

enum class RestorePolicy : uint8_t { SkipExisting, Overwrite, KeepBoth };

struct RestoreProgress {
    std::mutex mutex;
    size_t done = 0, total = 0, restored = 0, skipped = 0, failed = 0;
    uint64_t bytes = 0;
    std::string current;
    std::vector<std::string> errors; // first few
    bool finished = false;
};

// Pushes each item (verified by MD5 on the phone, mtime restored), then re-indexes the gallery.
void RunRestore(const AdbClient& adb, const std::string& serial, const std::vector<RestoreItem>& items, RestorePolicy policy,
    RestoreProgress& progress, std::stop_token st);

} // namespace ps
