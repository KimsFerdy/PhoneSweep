#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

struct PullResult {
    bool ok = false;
    bool deviceGone = false;
    uint64_t bytes = 0;
    std::string md5; // of what was written locally
    std::string error;
};

// Streams `adb exec-out cat` into `local` (binary-safe), computing MD5 on the fly.
PullResult PullToFile(const AdbClient& adb, const std::string& serial, const std::string& devicePath, const std::filesystem::path& local,
    std::stop_token st, const std::function<void(uint64_t bytes)>& onBytes = {});

// md5sum on the phone; nullopt if unreadable.
std::optional<std::string> PhoneMd5(const AdbClient& adb, const std::string& serial, const std::string& path, std::stop_token st);

// Pushes `local` to `devicePath` safely: upload to a hidden temp name in the same
// folder, check its MD5 on the phone, then move it into place and set the
// modification time (unix seconds, 0 = leave). Returns "" on success, else the reason.
std::string PushVerified(const AdbClient& adb, const std::string& serial, const std::filesystem::path& local, const std::string& devicePath,
    const std::string& localMd5, int64_t mtime, std::stop_token st);

// Ask MediaStore to (re)index these files so the gallery shows them correctly.
void MediaScan(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths, std::stop_token st);

} // namespace ps
