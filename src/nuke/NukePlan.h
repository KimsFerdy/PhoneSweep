#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class FileStore;
class VerifiedIndex;
struct AppInfo;

// Settings → Nuke Protector (user decision, replaces the spec's locked rule).
enum class ProtectorMode : uint8_t {
    Prompt, // only files saved + verified on the PC can be deleted (default)
    Free,   // whatever is in the plan is deleted permanently
};

// Why a file is or isn't deleted. Order = display order of the groups.
enum class NukeDecision : uint8_t {
    Delete,
    KeepUnsaved,  // marked KEEP but not verified on the PC (Prompt: blocker; Free: left on phone)
    NotSaved,     // Prompt mode: not verified on the PC, so it stays
    Protected,    // Android/data, obb, dot-folders: never implicitly
    Excluded,     // user unticked it in the review list
};

struct NukeFileFacts {
    bool keep = false;               // FileFlag_Keep
    bool verified = false;           // saved + verified, unchanged since (size + mtime)
    bool isProtected = false;        // protected folder
    bool userExcluded = false;       // unticked in review
    bool userIncludedProtected = false; // ticked in the Protected review list (Free mode only)
};

// THE rule. Pure; exhaustively unit-tested.
NukeDecision DecideNuke(const NukeFileFacts& f, ProtectorMode mode);

struct NukeGroup {
    std::vector<uint32_t> files; // FileStore indices
    uint64_t bytes = 0;
};

struct NukePlan {
    ProtectorMode mode = ProtectorMode::Prompt;
    NukeGroup del, keepUnsaved, notSaved, protectedFiles, excluded;
    std::vector<std::string> uninstall; // packages
    bool clearCaches = true;

    // Prompt mode: KEEP-but-unsaved files must be resolved before nuking.
    bool HasBlockers() const { return mode == ProtectorMode::Prompt && !keepUnsaved.files.empty(); }
    bool Empty() const { return del.files.empty() && uninstall.empty(); }
    uint64_t Fingerprint() const; // changes whenever what would be deleted changes
};

struct NukeOverrides {
    std::set<std::string, std::less<>> excludedPaths;          // unticked files
    std::set<std::string, std::less<>> includedProtectedPaths; // ticked protected files (Free)
    std::set<std::string, std::less<>> excludedApps;           // unticked apps
};

NukePlan BuildNukePlan(const FileStore& files, const VerifiedIndex& verified, const std::vector<AppInfo>& apps,
    const std::set<std::string, std::less<>>& uninstallSet, const NukeOverrides& ov, ProtectorMode mode);

// Plain-text plan for the dry run / log (every path listed).
std::string FormatNukePlan(const NukePlan& plan, const FileStore& files, std::string_view deviceName);

// Folders that may have become empty, deepest first. Never a volume root, a
// top-level folder (DCIM, Download, ...), anything under Android/, or the
// camera/screenshot folders. rmdir only removes empty ones anyway.
std::vector<std::string> CandidateEmptyDirs(const std::vector<std::string>& deletedPaths, const std::vector<std::string>& volumeRoots);

// SQL string literal for MediaStore cleanup: ' -> ''.
std::string SqlQuote(std::string_view s);

} // namespace ps
