#pragma once
#include "transfer/Manifest.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

inline constexpr const char* kManifestName = "saved_manifest.json";

// "<root>/SM-G960F_2026-11-11" -> "<root>/SM-G960F_2026-11-11.zip" (or " (2).zip" if taken).
std::filesystem::path ZipPathFor(const std::filesystem::path& folder);

struct ZipProgress {
    std::mutex mutex;
    std::string phase; // "Zipping", "Verifying", "Removing the folder"
    std::string current;
    uint64_t done = 0, total = 0; // bytes
    bool finished = false, ok = false;
    std::string error;
    std::filesystem::path zip;
    size_t files = 0;
    bool folderRemoved = false;
};

// Packs a backup folder (manifest first) into a .zip next to it, then re-reads the
// whole archive: every entry must pass its CRC and every manifest file must match its
// MD5. Written as .zip.part and renamed only when verified. The folder is deleted only
// if `deleteFolder` AND verification passed.
void ZipBackupFolder(const std::filesystem::path& folder, bool deleteFolder, ZipProgress& progress, std::stop_token st);

// The manifest stored inside a backup zip (nullopt if it isn't one).
struct ZipManifest {
    std::string serial, model;
    std::vector<ManifestEntry> entries;
};
std::optional<ZipManifest> ReadZipManifest(const std::filesystem::path& zip);

} // namespace ps
