#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

struct NukeApp {
    std::string package;
    std::vector<std::string> apkPaths;
};

struct NukeTask {
    std::string serial;
    std::vector<std::string> deletePaths;
    std::vector<std::string> volumeRoots;     // "/storage/emulated/0/", "/storage/0000-0000/"
    std::vector<std::string> volumeLabels;
    std::vector<NukeApp> apps;
    bool backupApks = true;
    std::filesystem::path apkBackupDir;       // <backup folder>/apks
    bool clearCaches = true;
    bool removeEmptyFolders = true;
    bool mediaRescan = true;
    std::filesystem::path tmpDir;             // local temp for list files
};

enum class NukePhase : int { Starting, BackupApks, DeleteFiles, RemoveFolders, Uninstall, ClearCaches, MediaRescan, Done };
const char* PhaseName(NukePhase p);

struct VolumeSpace {
    std::string label;
    uint64_t total = 0, used = 0, free = 0;
};

// Shared between the nuke job and the UI.
struct NukeProgress {
    mutable std::mutex mutex;
    NukePhase phase = NukePhase::Starting;
    size_t phaseDone = 0, phaseTotal = 0;
    bool finished = false;
    bool cancelled = false;
    std::string error;

    size_t deleted = 0;
    size_t deleteFailed = 0;
    std::vector<std::string> failedSamples; // a few paths that survived
    size_t foldersRemoved = 0;
    size_t apksBackedUp = 0;
    std::vector<std::string> uninstalled;
    std::vector<std::pair<std::string, std::string>> uninstallFailed; // package, reason
    bool cachesCleared = false;
    std::vector<VolumeSpace> before, after;
    std::vector<std::string> deletedPaths; // for the UI to drop from its file list
};

// Executes a nuke. Every step is logged. Cancellation is honoured between batches.
void RunNuke(const AdbClient& adb, const NukeTask& task, NukeProgress& progress, std::stop_token st);

// Removes MediaStore rows for deleted files so the gallery doesn't show ghosts.
void CleanMediaStore(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& deletedPaths, std::stop_token st);

// Deletes already-verified files right after saving ("Auto nuke saved files").
// Returns paths that are gone from the phone afterwards.
std::vector<std::string> DeleteFiles(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths,
    const std::filesystem::path& tmpDir, std::stop_token st);

} // namespace ps
