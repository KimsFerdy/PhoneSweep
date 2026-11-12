#include "update/Update.h"

#include "update/Http.h"
#include "util/JobSystem.h"
#include "util/Log.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <msi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>
#include <vector>

namespace ps {

namespace {

// Our MSI's UpgradeCode (installer/PhoneSweep.wxs). Stays the same for every version.
constexpr const wchar_t* kMsiUpgradeCode = L"{3B6E1A42-7D2C-4E8F-A5B1-9C0D4F2E6A13}";

bool EndsWithI(std::string_view s, std::string_view suffix)
{
    if (s.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    return true;
}

std::wstring LowerW(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)))
        out = p;
    CoTaskMemFree(p);
    return out;
}

bool IsUnder(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    if (parent.empty())
        return false;
    std::wstring c = LowerW(child.lexically_normal().wstring()), p = LowerW(parent.lexically_normal().wstring());
    if (!p.ends_with(L'\\'))
        p += L'\\';
    return c.starts_with(p);
}

int64_t NowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class Sha256Hasher {
public:
    Sha256Hasher()
    {
        if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            m_alg = nullptr;
        else if (BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0) < 0)
            m_hash = nullptr;
    }
    ~Sha256Hasher()
    {
        if (m_hash)
            BCryptDestroyHash(m_hash);
        if (m_alg)
            BCryptCloseAlgorithmProvider(m_alg, 0);
    }
    bool Ok() const { return m_hash != nullptr; }
    bool Add(const void* p, size_t n)
    {
        return BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0) >= 0;
    }
    std::string Hex()
    {
        unsigned char d[32];
        if (BCryptFinishHash(m_hash, d, sizeof(d), 0) < 0)
            return {};
        std::string out;
        for (unsigned char b : d)
            out += std::format("{:02x}", b);
        return out;
    }

private:
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};

} // namespace

// ---- Versions and releases ------------------------------------------------------

std::string SemVer::Str() const { return std::format("{}.{}.{}", major, minor, patch); }

std::optional<SemVer> ParseSemVer(std::string_view s)
{
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
        s.remove_prefix(1);
    if (const size_t cut = s.find_first_of("-+ "); cut != std::string_view::npos)
        s = s.substr(0, cut);
    int parts[3] = { 0, 0, 0 };
    int n = 0;
    while (!s.empty() && n < 3) {
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), parts[n]);
        if (ec != std::errc() || parts[n] < 0)
            return std::nullopt;
        ++n;
        s.remove_prefix(static_cast<size_t>(ptr - s.data()));
        if (s.empty())
            break;
        if (s[0] != '.')
            return std::nullopt;
        s.remove_prefix(1);
    }
    if (n < 2 || !s.empty())
        return std::nullopt;
    return SemVer{ parts[0], parts[1], parts[2] };
}

std::optional<ReleaseInfo> ParseRelease(std::string_view json, std::string* error)
{
    try {
        const auto j = nlohmann::json::parse(json);
        ReleaseInfo r;
        r.tag = j.value("tag_name", "");
        const auto v = ParseSemVer(r.tag);
        if (!v) {
            if (error)
                *error = "Release has no version tag";
            return std::nullopt;
        }
        r.version = *v;
        r.title = j.value("name", "");
        if (r.title.empty())
            r.title = "PhoneSweep " + r.tag;
        if (j.contains("body") && j["body"].is_string())
            r.notes = j["body"].get<std::string>();
        r.pageUrl = j.value("html_url", "");
        if (!r.pageUrl.starts_with("https://github.com/"))
            r.pageUrl = "https://github.com/KimsFerdy/PhoneSweep/releases";
        r.publishedAt = j.value("published_at", "").substr(0, 10);
        if (j.contains("assets") && j["assets"].is_array())
            for (const auto& a : j["assets"]) {
                ReleaseAsset asset;
                asset.name = a.value("name", "");
                asset.url = a.value("browser_download_url", "");
                asset.size = a.value("size", uint64_t{ 0 });
                const std::string digest = a.contains("digest") && a["digest"].is_string() ? a["digest"].get<std::string>() : "";
                if (digest.starts_with("sha256:") && digest.size() == 7 + 64)
                    asset.sha256 = digest.substr(7);
                std::ranges::transform(asset.sha256, asset.sha256.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                if (asset.name.empty() || !asset.url.starts_with(kReleaseDownloadPrefix) || asset.name.find_first_of("/\\:") != std::string::npos)
                    continue; // never download from anywhere else
                r.assets.push_back(std::move(asset));
            }
        return r;
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("Unexpected answer from GitHub: ") + e.what();
        return std::nullopt;
    }
}

namespace {

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::string Inline(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            ++i; // **bold** / __bold__
            continue;
        }
        if (c == '`')
            continue;
        if (c == '[') {
            // [text](url) -> text
            const size_t close = s.find(']', i);
            if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
                const size_t end = s.find(')', close);
                if (end != std::string_view::npos) {
                    out += Inline(s.substr(i + 1, close - i - 1));
                    i = end;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

} // namespace

std::vector<NoteLine> FormatReleaseNotes(std::string_view md)
{
    std::vector<NoteLine> out;
    size_t pos = 0;
    while (pos <= md.size()) {
        const size_t nl = md.find('\n', pos);
        const std::string_view raw = md.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? md.size() + 1 : nl + 1;
        const std::string_view line = Trim(raw);
        if (line.empty()) {
            if (!out.empty() && out.back().kind != NoteLine::Kind::Blank)
                out.push_back({ NoteLine::Kind::Blank, {} });
            continue;
        }
        if (line.starts_with("#")) {
            const size_t hashes = line.find_first_not_of('#');
            out.push_back({ NoteLine::Kind::Heading, Inline(Trim(line.substr(hashes == std::string_view::npos ? line.size() : hashes))) });
        } else if (line.starts_with("- ") || line.starts_with("* ") || line.starts_with("+ ")) {
            out.push_back({ NoteLine::Kind::Bullet, Inline(Trim(line.substr(2))) });
        } else if (line.starts_with("|")) {
            if (line.find_first_not_of("|-: ") == std::string_view::npos)
                continue; // |---|---| separator
            std::string row;
            size_t c = 1;
            while (c < line.size()) {
                const size_t bar = line.find('|', c);
                const std::string_view cell = Trim(line.substr(c, bar == std::string_view::npos ? std::string_view::npos : bar - c));
                if (!cell.empty())
                    row += (row.empty() ? "" : "  \xC2\xB7  ") + Inline(cell);
                if (bar == std::string_view::npos)
                    break;
                c = bar + 1;
            }
            out.push_back({ NoteLine::Kind::Text, row });
        } else {
            out.push_back({ NoteLine::Kind::Text, Inline(line) });
        }
    }
    while (!out.empty() && out.back().kind == NoteLine::Kind::Blank)
        out.pop_back();
    return out;
}

const char* InstallKindName(InstallKind k)
{
    switch (k) {
    case InstallKind::InnoUser: return "Installer (this user)";
    case InstallKind::InnoAllUsers: return "Installer (all users)";
    case InstallKind::Msi: return "MSI package (all users)";
    default: return "Portable (zip)";
    }
}

InstallKind ParseInstallKind(std::string_view s, InstallKind fallback)
{
    if (s == "portable") return InstallKind::Portable;
    if (s == "inno" || s == "setup") return InstallKind::InnoUser;
    if (s == "inno-all") return InstallKind::InnoAllUsers;
    if (s == "msi") return InstallKind::Msi;
    return fallback;
}

const ReleaseAsset* PickAsset(const ReleaseInfo& r, InstallKind k)
{
    const std::string_view suffix = k == InstallKind::Msi ? ".msi" : k == InstallKind::Portable ? "-win64.zip" : "-setup.exe";
    for (const auto& a : r.assets)
        if (EndsWithI(a.name, suffix))
            return &a;
    return nullptr;
}

InstallKind DetectInstallKind(const std::filesystem::path& exeDir)
{
    std::error_code ec;
    bool inno = false;
    for (const auto& entry : std::filesystem::directory_iterator(exeDir, ec)) {
        const std::wstring name = LowerW(entry.path().filename().wstring());
        if (name.starts_with(L"unins") && name.ends_with(L".exe")) {
            inno = true;
            break;
        }
    }
    const bool programFiles = IsUnder(exeDir, KnownFolder(FOLDERID_ProgramFiles)) || IsUnder(exeDir, KnownFolder(FOLDERID_ProgramFilesX86));
    if (inno)
        return programFiles ? InstallKind::InnoAllUsers : InstallKind::InnoUser;
    wchar_t product[39]{};
    if (programFiles && MsiEnumRelatedProductsW(kMsiUpgradeCode, 0, 0, product) == ERROR_SUCCESS)
        return InstallKind::Msi;
    return InstallKind::Portable;
}

std::string Sha256(std::string_view data)
{
    Sha256Hasher h;
    if (!h.Ok() || !h.Add(data.data(), data.size()))
        return {};
    return h.Hex();
}

std::string Sha256File(const std::filesystem::path& file, std::stop_token st)
{
    std::ifstream in(file, std::ios::binary);
    Sha256Hasher h;
    if (!in || !h.Ok())
        return {};
    std::vector<char> buf(1 << 20);
    while (in) {
        if (st.stop_requested())
            return {};
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() > 0 && !h.Add(buf.data(), static_cast<size_t>(in.gcount())))
            return {};
    }
    return h.Hex();
}

InstallerLaunch MakeInstallerLaunch(InstallKind k, const std::filesystem::path& installer)
{
    const std::filesystem::path log = installer.parent_path() / L"install.log";
    InstallerLaunch l;
    if (k == InstallKind::Msi) {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        l.exe = std::filesystem::path(sys) / L"msiexec.exe";
        // /passive: progress bar only. PS_RELAUNCH starts PhoneSweep again at the end (see PhoneSweep.wxs).
        l.params = std::format(L"/i \"{}\" /passive /norestart PS_RELAUNCH=1 /l*v \"{}\"", installer.wstring(), log.wstring());
    } else {
        // /SILENT: progress window, no questions. /RELAUNCH=1 starts PhoneSweep again (see PhoneSweep.iss).
        l.exe = installer;
        l.params = std::format(L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS {} /RELAUNCH=1 /LOG=\"{}\"",
            k == InstallKind::InnoAllUsers ? L"/ALLUSERS" : L"/CURRENTUSER", log.wstring());
    }
    return l;
}

bool LaunchInstaller(const InstallerLaunch& l, std::string* error)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = l.exe.c_str();
    sei.lpParameters = l.params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        if (error)
            *error = GetLastError() == ERROR_CANCELLED ? "The installer needs administrator rights (cancelled)." : std::format("Couldn't start the installer (error {})", GetLastError());
        return false;
    }
    LogInfo("Update: started {} {}", Narrow(l.exe.wstring()), Narrow(l.params));
    return true;
}

// ---- Updater ----------------------------------------------------------------------

Updater::Updater(JobSystem& jobs, SemVer current, InstallKind kind)
    : m_jobs(jobs)
    , m_current(current)
    , m_kind(kind)
{
}

Updater::~Updater()
{
    if (m_job)
        m_job->Cancel();
}

std::filesystem::path Updater::DownloadDir()
{
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / L"PhoneSweep-update";
}

void Updater::CleanDownloads()
{
    std::error_code ec;
    std::filesystem::remove_all(DownloadDir(), ec);
}

Updater::Status Updater::Get() const
{
    std::lock_guard l(m_shared->mutex);
    return m_shared->status;
}

void Updater::Cancel()
{
    if (m_job)
        m_job->Cancel();
}

void Updater::Check(bool automatic)
{
    {
        std::lock_guard l(m_shared->mutex);
        auto& s = m_shared->status;
        if (s.state == State::Checking || s.state == State::Downloading)
            return;
        s.state = State::Checking;
        s.automatic = automatic;
        s.message.clear();
    }
    auto shared = m_shared;
    const SemVer current = m_current;
    m_job = m_jobs.Submit("Checking for updates", automatic ? JobPriority::Low : JobPriority::Normal, [shared, current](Job&, std::stop_token st) {
        const HttpResult r = HttpGet(kLatestReleaseApi, st);
        std::string err;
        std::optional<ReleaseInfo> rel;
        if (r.ok)
            rel = ParseRelease(r.body, &err);
        std::lock_guard l(shared->mutex);
        auto& s = shared->status;
        s.checkedAt = NowUnix();
        if (!r.ok || !rel) {
            s.state = r.cancelled ? State::Idle : State::Error;
            s.message = r.ok ? err : r.error;
            if (!r.ok && r.status == 404)
                s.message = "No release published yet.";
            LogWarn("Update check failed: {}", s.message);
            return;
        }
        LogInfo("Update check: latest {} (running {})", rel->version.Str(), current.Str());
        s.state = rel->version > current ? State::Available : State::UpToDate;
        s.release = std::move(rel);
    });
}

void Updater::Download()
{
    ReleaseAsset asset;
    {
        std::lock_guard l(m_shared->mutex);
        auto& s = m_shared->status;
        if (s.state != State::Available && s.state != State::Error)
            return;
        if (!s.release)
            return;
        const ReleaseAsset* a = PickAsset(*s.release, m_kind);
        if (!a) {
            s.state = State::Error;
            s.message = "This release has no installer for your kind of installation.";
            return;
        }
        asset = *a;
        s.state = State::Downloading;
        s.done = 0;
        s.total = asset.size;
        s.message.clear();
    }
    auto shared = m_shared;
    m_job = m_jobs.Submit("Downloading update", JobPriority::Normal, [shared, asset](Job& job, std::stop_token st) {
        const auto file = DownloadDir() / Widen(asset.name);
        auto finish = [&](State state, std::string msg) {
            std::lock_guard l(shared->mutex);
            auto& s = shared->status;
            s.state = state;
            s.message = std::move(msg);
            if (state == State::Ready)
                s.installer = file;
        };
        auto verify = [&]() -> std::string {
            std::error_code ec;
            if (asset.size && std::filesystem::file_size(file, ec) != asset.size)
                return "The download has the wrong size.";
            if (!asset.sha256.empty() && Sha256File(file, st) != asset.sha256)
                return "The download doesn't match GitHub's checksum.";
            return {};
        };
        std::error_code ec;
        if (std::filesystem::exists(file, ec) && verify().empty()) {
            finish(State::Ready, {}); // already downloaded earlier
            return;
        }
        const HttpResult r = HttpDownload(asset.url, file, st, [&](uint64_t done, uint64_t total) {
            job.SetProgress(total ? static_cast<float>(done) / static_cast<float>(total) : -1.0f);
            std::lock_guard l(shared->mutex);
            shared->status.done = done;
            if (total)
                shared->status.total = total;
        });
        if (!r.ok) {
            finish(r.cancelled ? State::Available : State::Error, r.cancelled ? std::string() : r.error);
            return;
        }
        if (const std::string bad = verify(); !bad.empty()) {
            std::filesystem::remove(file, ec);
            LogWarn("Update download rejected: {}", bad);
            finish(State::Error, bad + " It was deleted; please try again.");
            return;
        }
        LogInfo("Update downloaded and verified: {} ({} bytes, sha256 {})", asset.name, asset.size, asset.sha256.empty() ? "n/a" : asset.sha256);
        finish(State::Ready, {});
    });
}

bool Updater::Install(std::string* error)
{
    Status s = Get();
    if (s.state != State::Ready || !s.release) {
        if (error)
            *error = "Nothing to install yet.";
        return false;
    }
    // Check again right before running it: the file sat in %TEMP% meanwhile.
    if (const ReleaseAsset* a = PickAsset(*s.release, m_kind); a && !a->sha256.empty() && Sha256File(s.installer) != a->sha256) {
        if (error)
            *error = "The downloaded installer changed on disk. Download it again.";
        std::lock_guard l(m_shared->mutex);
        m_shared->status.state = State::Error;
        m_shared->status.message = *error;
        return false;
    }
    return LaunchInstaller(MakeInstallerLaunch(m_kind, s.installer), error);
}

} // namespace ps
