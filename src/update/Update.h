#pragma once
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

class JobSystem;
class Job;
using JobPtr = std::shared_ptr<Job>;

// ---- Versions and releases (pure, unit-tested) --------------------------------

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const SemVer&) const = default;
    std::string Str() const;
};
// "v1.2.3", "1.2.3", "1.2" (patch 0). A "-beta" style suffix is ignored.
std::optional<SemVer> ParseSemVer(std::string_view s);

struct ReleaseAsset {
    std::string name;
    std::string url; // always under kReleaseDownloadPrefix
    uint64_t size = 0;
    std::string sha256; // lowercase hex from GitHub's "digest", empty if GitHub didn't give one
};

struct ReleaseInfo {
    std::string tag;
    SemVer version;
    std::string title;
    std::string notes; // Markdown body
    std::string pageUrl;
    std::string publishedAt; // "2026-11-12"
    std::vector<ReleaseAsset> assets;
};

inline constexpr const char* kLatestReleaseApi = "https://api.github.com/repos/KimsFerdy/PhoneSweep/releases/latest";
inline constexpr const char* kReleaseDownloadPrefix = "https://github.com/KimsFerdy/PhoneSweep/releases/download/";

// GitHub "latest release" JSON. Assets pointing anywhere but our release downloads are dropped.
std::optional<ReleaseInfo> ParseRelease(std::string_view json, std::string* error = nullptr);

// Release notes (GitHub Markdown) flattened for display: headings, bullets, table rows
// ("a · b"), blank lines; **bold**, `code` and [links](url) reduced to their text.
struct NoteLine {
    enum class Kind : uint8_t { Text, Heading, Bullet, Blank };
    Kind kind = Kind::Text;
    std::string text;
};
std::vector<NoteLine> FormatReleaseNotes(std::string_view markdown);

// How this copy of PhoneSweep was installed, which decides what an update downloads.
enum class InstallKind : uint8_t {
    Portable,     // unzipped: updates are downloaded by hand
    InnoUser,     // setup.exe, current user (no admin)
    InnoAllUsers, // setup.exe, all users (Program Files)
    Msi,          // the .msi (all users)
};
const char* InstallKindName(InstallKind k);
InstallKind ParseInstallKind(std::string_view s, InstallKind fallback);

// The asset an update uses: *-setup.exe, *.msi or *-win64.zip. Null if missing.
const ReleaseAsset* PickAsset(const ReleaseInfo& r, InstallKind k);

// Looks next to the exe: unins*.exe = Inno Setup (all users when under Program Files);
// otherwise our MSI's UpgradeCode registered + Program Files = MSI; else portable.
InstallKind DetectInstallKind(const std::filesystem::path& exeDir);

// Lowercase hex SHA-256 (Windows CNG). Empty on failure / cancel.
std::string Sha256(std::string_view data);
std::string Sha256File(const std::filesystem::path& file, std::stop_token st = {});

// Silent install command. The new installer starts PhoneSweep again when it's done.
struct InstallerLaunch {
    std::filesystem::path exe;
    std::wstring params;
};
InstallerLaunch MakeInstallerLaunch(InstallKind k, const std::filesystem::path& installer);
bool LaunchInstaller(const InstallerLaunch& l, std::string* error);

// ---- Updater: check / download state machine (UI polls Get()) -----------------

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Error };
    struct Status {
        State state = State::Idle;
        std::string message; // error text, or "Checked just now"
        std::optional<ReleaseInfo> release;
        uint64_t done = 0, total = 0; // download progress
        std::filesystem::path installer; // when Ready
        bool automatic = false;          // the last check was the startup check
        int64_t checkedAt = 0;           // unix seconds
    };

    Updater(JobSystem& jobs, SemVer current, InstallKind kind);
    ~Updater();

    void Check(bool automatic);
    void Download();
    void Cancel();
    Status Get() const;
    // Starts the installer; on success the caller closes PhoneSweep right away.
    bool Install(std::string* error);

    SemVer Current() const { return m_current; }
    InstallKind Kind() const { return m_kind; }
    static std::filesystem::path DownloadDir(); // %TEMP%/PhoneSweep-update
    static void CleanDownloads();               // removes leftovers of earlier updates

private:
    struct Shared {
        mutable std::mutex mutex;
        Status status;
    };
    JobSystem& m_jobs;
    SemVer m_current;
    InstallKind m_kind;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    JobPtr m_job;
};

} // namespace ps
