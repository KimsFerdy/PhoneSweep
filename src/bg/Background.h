#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class AdbClient;

// ---- Background state -------------------------------------------------------

struct BgState {
    std::string mode;      // RUN_ANY_IN_BACKGROUND: "allow", "ignore", "default"...
    bool explicitMode = false; // false = "Default mode:" line (nothing set for this app)
    int bucket = -1;       // standby bucket (10 active ... 45 restricted, 50 never)
    bool BackgroundAllowed() const { return mode != "ignore" && mode != "deny"; }
};

// Output of the batched query script: "@@pkg" header, appops output, bucket number.
std::map<std::string, BgState, std::less<>> ParseBgBatch(std::string_view text);
const char* BucketName(int bucket); // "Active", "Working set", "Frequent", "Rare", "Restricted", "Never", "—"

// One adb call for all packages.
std::map<std::string, BgState, std::less<>> QueryBackground(const AdbClient& adb, std::string_view serial,
    const std::vector<std::string>& packages, std::stop_token st);

// ---- Running processes ------------------------------------------------------

struct ProcessEntry {
    int pid = 0;
    uint64_t rssKb = 0;
    std::string user;
    std::string name; // "com.whatsapp" or "com.whatsapp:pushservice"
};
// `ps -A -o PID,RSS,USER,NAME`, app processes only (users like u0_a123).
std::vector<ProcessEntry> ParsePsApps(std::string_view text);

struct RunningApp {
    std::string package;
    uint64_t rssKb = 0;
    int processes = 0;
};
// Groups processes by package (name before ':'), biggest RAM first.
std::vector<RunningApp> GroupByPackage(const std::vector<ProcessEntry>& procs);

// ---- Restrictions (reversible) ------------------------------------------------

struct Restriction {
    std::string serial;
    std::string package;
    BgState before;       // what to restore on Undo
    int appliedBucket = 0;
    std::string appliedAt; // ISO local time, for display
};

// %LOCALAPPDATA%/PhoneSweep/restrictions.json: every Restrict we did, so it can be undone.
class RestrictionLog {
public:
    explicit RestrictionLog(std::filesystem::path file);

    std::vector<Restriction> ForDevice(std::string_view serial) const;
    std::vector<Restriction> All() const;
    std::optional<Restriction> Find(std::string_view serial, std::string_view pkg) const;
    void Add(Restriction r);    // replaces an existing entry for the same device+package
    void Remove(std::string_view serial, std::string_view pkg);
    size_t Count() const;

private:
    void Save() const; // caller holds the lock
    std::filesystem::path m_file;
    mutable std::mutex m_mutex;
    std::vector<Restriction> m_entries;
};

struct RestrictResult {
    bool ok = false;
    std::string error;
    Restriction record; // valid when ok
};

// Restrict = RUN_ANY_IN_BACKGROUND ignore + lowest standby bucket the OS accepts
// ("restricted" on Android 12+, "rare" before). Records prior state first.
RestrictResult RestrictBackground(const AdbClient& adb, std::string_view serial, std::string_view pkg, int deviceSdk,
    std::stop_token st);
// Restores exactly what Restrict recorded.
// serial = adb transport; r.serial = the phone's hardware serial (record key).
bool UndoRestriction(const AdbClient& adb, std::string_view serial, const Restriction& r, std::string* error, std::stop_token st);

} // namespace ps
