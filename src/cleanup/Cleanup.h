#pragma once
#include "nuke/NukePlan.h"
#include "scan/FileModel.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

class AdbClient;
class FileStore;
class VerifiedIndex;

// ---- Duplicates ----------------------------------------------------------------

struct DupFile {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
};

struct DupGroup {
    uint64_t size = 0;
    std::string md5;            // full-file checksum shared by every copy
    std::vector<DupFile> files; // >= 2 identical copies
    size_t keep = 0;            // index into files: the copy that stays
    std::set<std::string, std::less<>> alsoKeep; // v1.1: more copies the user chose to keep (paths)

    bool Keeps(size_t i) const { return i == keep || (i < files.size() && alsoKeep.contains(files[i].path)); }
    size_t KeptCount() const;
    uint64_t Reclaimable() const { return size * (files.size() - std::min(files.size(), KeptCount())); }
    // Marks copy i as kept or deleted. One copy always stays: un-keeping the last kept
    // copy is refused (returns false). Un-keeping the main keeper promotes another kept copy.
    bool SetKept(size_t i, bool kept);
};

// Same-size candidate groups (>= 2 files of size >= minSize), protected files excluded.
std::vector<std::vector<uint32_t>> SizeCandidates(const FileStore& files, uint64_t minSize);

// Which copy to keep: KEEP-marked > Camera/original source > not messaging media >
// internal storage > oldest > shortest path. Returns an index into `group`.
size_t ChooseKeeper(const std::vector<uint32_t>& group, const FileStore& files);

// Paths to delete for the ticked groups: every copy except the keeper. Never
// returns all copies of a group (the keeper is excluded by construction, and a
// KEEP-marked copy is never deleted). Pure; unit-tested.
std::vector<std::string> DuplicateDeleteList(const std::vector<DupGroup>& groups, const std::vector<bool>& ticked,
    const FileStore& files);

struct DupProgress {
    std::string phase;
    size_t done = 0, total = 0;
};

// Hashes on the phone. partial=true: first 64 KB only. One result per path, "-" if unreadable.
std::vector<std::string> HashOnPhone(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths, bool partial,
    std::stop_token st, const std::function<void(size_t done)>& onProgress);

// Full pipeline: size groups -> 64 KB hashes -> full MD5. Only proven-identical groups come back.
std::vector<DupGroup> FindDuplicates(const AdbClient& adb, const std::string& serial, const FileStore& snapshot, uint64_t minSize,
    std::stop_token st, const std::function<void(const DupProgress&)>& onProgress);

// ---- Junk ------------------------------------------------------------------------

enum class JunkKind : uint8_t {
    Thumbnails,
    AppCaches,
    Leftovers,
    WhatsAppStatuses,
    LogsTemp,
    OldChatBackups,
    Installers,
    EmptyFiles,
    Count
};
inline constexpr size_t kJunkKindCount = static_cast<size_t>(JunkKind::Count);

struct JunkInfo {
    JunkKind kind;
    const char* name;
    const char* why;
    bool safe; // regenerable / useless: allowed even in Nuke prompt mode
};
const JunkInfo& GetJunkInfo(JunkKind k);

// installed: every package on the phone (user + system), for leftover detection.
// newestBackups: paths of the newest msgstore backup per WhatsApp folder (never junk).
std::optional<JunkKind> ClassifyJunk(const FileEntry& e, const std::set<std::string, std::less<>>& installed);

struct JunkCategory {
    JunkKind kind;
    std::vector<uint32_t> files;   // deletable under the current protector mode
    std::vector<uint32_t> blocked; // Review junk that Nuke prompt protects (not saved)
    uint64_t bytes = 0, blockedBytes = 0;
};

// Junk per category, honouring KEEP (never) and the protector mode.
std::vector<JunkCategory> BuildJunk(const FileStore& files, const VerifiedIndex& verified, const std::set<std::string, std::less<>>& installed,
    ProtectorMode mode);

} // namespace ps
