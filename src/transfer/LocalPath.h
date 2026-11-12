#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ps {

// Makes one path component safe on NTFS: replaces <>:"|?*\ and control chars,
// fixes trailing dots/spaces, prefixes reserved names (CON, NUL, COM1...),
// and turns invalid UTF-8 bytes into '_'. Never returns "" / "." / "..".
std::string SanitizeComponent(std::string_view name);

// Device-relative path ("DCIM/Camera/a.jpg") -> safe relative Windows path.
// volumePrefix ("" for internal, "SD card" for the SD) becomes the first folder.
std::filesystem::path MapToLocal(std::string_view volumePrefix, std::string_view relPath, bool preserveStructure);

enum class ConflictPolicy { SkipIfIdentical, Rename, Overwrite };
const char* ToString(ConflictPolicy p);

// "a.jpg" -> "a (1).jpg", "a (2).jpg"... first name that doesn't exist.
std::filesystem::path NextFreeName(const std::filesystem::path& p);

// Adds the \\?\ prefix to absolute paths that are too long for MAX_PATH APIs.
std::filesystem::path LongPath(const std::filesystem::path& p);

// Expands a folder pattern: {device} {model} {date} {time} -> "SM-G960F_2026-11-11".
std::string ExpandFolderPattern(std::string_view pattern, std::string_view device, int64_t unixTime);

// The user's Documents folder (Known Folder), or the profile dir as a fallback.
std::filesystem::path DocumentsFolder();

// Sets a file's modification time from unix seconds.
bool SetFileMtime(const std::filesystem::path& p, int64_t unixSeconds);

// Free bytes on the volume holding `dir` (0 if unknown).
uint64_t FreeSpace(const std::filesystem::path& dir);

} // namespace ps
