#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

// One file that was saved to the PC AND verified. Nuke only trusts these.
struct ManifestEntry {
    std::string devicePath;  // raw device bytes (may be invalid UTF-8)
    uint64_t size = 0;
    int64_t mtime = 0;       // device mtime at the time of the copy
    std::string md5;         // "" when verified by size only
    std::string localRel;    // UTF-8 path relative to the manifest's folder
    std::string verifiedAt;  // local ISO time
    std::string kind = "file"; // "file" | "apk"

    std::string Method() const { return md5.empty() ? "size" : "size+md5"; }
};

// <backup folder>/saved_manifest.json. Written atomically (tmp + rename).
class Manifest {
public:
    Manifest(std::filesystem::path file, std::string serial, std::string model);

    bool Load(); // false if missing/unreadable (starts empty)
    void Upsert(ManifestEntry e);
    bool Save() const;

    const std::filesystem::path& File() const { return m_file; }
    const std::string& Serial() const { return m_serial; }
    std::vector<ManifestEntry> Entries() const;
    size_t Size() const;

    // JSON <-> entries (public for tests).
    static std::string ToJson(const std::string& serial, const std::string& model, const std::vector<ManifestEntry>& entries);
    static bool FromJson(std::string_view json, std::string* serial, std::vector<ManifestEntry>* entries);

private:
    std::filesystem::path m_file;
    std::string m_serial, m_model;
    mutable std::mutex m_mutex;
    std::map<std::string, ManifestEntry> m_entries; // by devicePath
};

// Everything verified for one device across all backup folders under a root.
class VerifiedIndex {
public:
    void Clear();
    // Scans <root>/*/saved_manifest.json for manifests of `serial`.
    size_t LoadForDevice(const std::filesystem::path& root, std::string_view serial);
    void Add(const ManifestEntry& e, const std::filesystem::path& folder);

    // Verified AND unchanged since (same size + mtime). This is the Nuke rule.
    bool IsVerified(std::string_view devicePath, uint64_t size, int64_t mtime) const;
    std::optional<ManifestEntry> Find(std::string_view devicePath) const;
    std::optional<std::filesystem::path> LocalFile(std::string_view devicePath) const;
    size_t Size() const { return m_entries.size(); }
    uint64_t Version() const { return m_version; }

private:
    struct Item {
        ManifestEntry entry;
        std::filesystem::path folder;
    };
    std::map<std::string, Item, std::less<>> m_entries;
    uint64_t m_version = 0;
};

bool IsValidUtf8(std::string_view s);

} // namespace ps
