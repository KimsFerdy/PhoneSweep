#pragma once
#include "bg/Usage.h"
#include "scan/AppModel.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class AdbClient;
class FileStore;
class VerifiedIndex;

// https://play.google.com/store/apps/details?id=<pkg>; "" if the package name isn't valid.
std::string PlayStoreUrl(std::string_view package);

// Text for any AI assistant: a short question + everything PhoneSweep knows about the
// app (permissions, installer, flags, size, live CPU/RAM, battery drain). AI-neutral.
std::string FormatAskAi(const AppInfo& app, const std::optional<AppUsage>& usage, std::string_view phone);

// The app's folders on shared storage, per volume: Android/data|media|obb/<pkg>/.
struct AppFolder {
    std::string path;      // "/storage/emulated/0/Android/media/com.whatsapp/"
    std::string kind;      // "data", "media", "obb"
    size_t files = 0, unsaved = 0;
    uint64_t bytes = 0, unsavedBytes = 0;
};
std::vector<AppFolder> AppStorageFolders(std::string_view package, const FileStore& files, const VerifiedIndex& verified);
// Nuke prompt: uninstalling is blocked while data/media folders hold files that aren't
// saved on the PC (OBB = game downloads, fetched again on reinstall, not counted).
size_t UnsavedAppFiles(const std::vector<AppFolder>& folders, uint64_t* bytes = nullptr);

enum class UninstallMode : uint8_t {
    Quick, // pm uninstall
    Full,  // pm clear (data + cache) + uninstall + delete leftover Android/data|media|obb folders
};

struct UninstallTask {
    std::string serial;
    AppInfo app;
    UninstallMode mode = UninstallMode::Quick;
    bool backupApk = true;
    std::filesystem::path apkDir;         // <backup folder>/apks
    std::vector<std::string> folders;     // the app's Android/data|media|obb folders (Full deletes them)
    std::vector<std::string> knownFiles;  // scanned files inside them (MediaStore cleanup)
};

struct UninstallProgress {
    std::mutex mutex;
    std::string step;
    bool finished = false, ok = false;
    std::string message;
    bool apkSaved = false, dataCleared = false;
    std::vector<std::string> removedFolders;
};

// APK backup (base + splits), size-checked against the phone (same check as Nuke).
bool BackupApk(const AdbClient& adb, const std::string& serial, const AppInfo& app, const std::filesystem::path& apkDir, std::stop_token st,
    std::string* error);

void RunUninstall(const AdbClient& adb, const UninstallTask& task, UninstallProgress& progress, std::stop_token st);

} // namespace ps
