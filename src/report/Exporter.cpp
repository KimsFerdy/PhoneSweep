#include "report/Exporter.h"

#include "app/Version.h"

#include "report/AppListFormat.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <unordered_map>

namespace ps {

namespace {

std::string Local(int64_t t, const char* fmt)
{
    const auto local = std::chrono::current_zone()->to_local(std::chrono::sys_seconds(std::chrono::seconds(t)));
    return std::vformat(fmt, std::make_format_args(local));
}

int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string Count(uint64_t n)
{
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<size_t>(i), ",");
    return s;
}

bool WriteFile(const std::filesystem::path& p, std::string_view data)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// "SD card · DCIM/Camera/a.jpg" or "DCIM/Camera/a.jpg".
std::string DisplayPath(const FileStore& s, const FileEntry& e)
{
    const std::string rel(e.RelPath());
    if (e.volume > 0 && e.volume < s.Volumes().size())
        return s.Volumes()[e.volume].label + ": " + rel;
    return rel;
}

std::string DisplayFolder(const FileStore& s, const FileEntry& e)
{
    std::string folder(e.RelFolder());
    if (folder.empty())
        folder = "/";
    if (e.volume > 0 && e.volume < s.Volumes().size())
        return s.Volumes()[e.volume].label + ": " + folder;
    return folder;
}

} // namespace

std::string MdCell(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        // Copy one valid UTF-8 sequence, or replace the byte.
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n > 1 && (i + n > s.size() || !IsValidUtf8(s.substr(i, n))))
            n = 0;
        if (n == 0) {
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++i;
            continue;
        }
        if (n == 1) {
            if (c == '|')
                out += "\\|";
            else if (c < 0x20)
                out += ' ';
            else
                out.push_back(static_cast<char>(c));
        } else {
            out.append(s.substr(i, n));
        }
        i += n;
    }
    return out;
}

std::string FormatUserAppList(const ReportInputs& in)
{
    if (!in.apps)
        return {};
    std::vector<const AppInfo*> user;
    for (const auto& a : *in.apps)
        if (!a.system)
            user.push_back(&a);
    // Riskiest first so the interesting apps are at the top of what the AI reads.
    std::ranges::stable_sort(user, [](const AppInfo* a, const AppInfo* b) { return a->risk > b->risk; });
    AppListOptions o;
    o.fullPermissions = in.fullPermissions;
    return FormatAppList(user, o);
}

std::string FormatScanReport(const ReportInputs& in)
{
    const int64_t now = in.now ? in.now : Now();
    const auto& d = in.device;
    std::string md;
    md += "# PhoneSweep scan report\n\n";
    md += std::format("Generated {} with {}.\n\n", Local(now, "{:%Y-%m-%d %H:%M}"), kAppNameVersion);
    md += "> Paste this file (and applist.txt) into any AI assistant (Claude, ChatGPT, Grok, Gemini, or an MCP client) and ask: "
          "what fills this phone, what is safe to delete, and which apps look suspicious?\n\n";

    // ---- Device ----
    md += "## Device\n\n| | |\n|---|---|\n";
    md += std::format("| Model | {} ({}) |\n", MdCell(d.displayName), MdCell(d.model));
    md += std::format("| Serial | {} |\n", MdCell(in.maskSerial ? MaskSerial(d.serial) : d.serial));
    md += std::format("| Android | {}{}{} |\n", MdCell(d.android), d.oneUi.empty() ? "" : std::format(" (One UI {})", d.oneUi),
        d.securityPatch.empty() ? "" : std::format(", security patch {}", d.securityPatch));
    if (d.ramMarketingGB)
        md += std::format("| RAM | {} GB |\n", *d.ramMarketingGB);
    if (d.batteryLevel)
        md += std::format("| Battery | {}%{} |\n", *d.batteryLevel, d.charging ? " (charging)" : "");
    md += "\n";

    const FileStore* fs = in.files;
    if (fs) {
        // ---- Storage ----
        md += "## Storage\n\n| Volume | Total | Used | Free | Used % |\n|---|---:|---:|---:|---:|\n";
        for (const auto& v : fs->Volumes()) {
            if (!v.totalBytes)
                continue;
            md += std::format("| {} | {} | {} | {} | {:.0f}% |\n", MdCell(v.label), FormatBytes(v.totalBytes), FormatBytes(v.usedBytes),
                FormatBytes(v.freeBytes), 100.0 * static_cast<double>(v.usedBytes) / static_cast<double>(v.totalBytes));
        }
        md += "\n";

        // ---- Categories ----
        md += "## Files by category\n\nShared storage and SD card; app-private folders are listed separately.\n\n";
        md += "| Category | Files | Size |\n|---|---:|---:|\n";
        for (size_t c = 0; c < kCategoryCount; ++c) {
            const auto& t = fs->ByCategory(static_cast<Category>(c));
            md += std::format("| {} | {} | {} |\n", CategoryName(static_cast<Category>(c)), Count(t.count), FormatBytes(t.bytes));
        }
        md += std::format("| **Total** | **{}** | **{}** |\n\n", Count(fs->All().count), FormatBytes(fs->All().bytes));
        md += std::format("Protected folders (Android/data, Android/obb, hidden dot-folders): {} files, {}.\n\n", Count(fs->Protected().count),
            FormatBytes(fs->Protected().bytes));

        // ---- Sources ----
        md += "## Files by source\n\n| Source | Files | Size |\n|---|---:|---:|\n";
        for (size_t s = 0; s < kSourceCount; ++s) {
            const auto& t = fs->BySource(static_cast<Source>(s));
            if (t.count)
                md += std::format("| {} | {} | {} |\n", SourceName(static_cast<Source>(s)), Count(t.count), FormatBytes(t.bytes));
        }
        md += "\n";

        // ---- Top 50 files ----
        std::vector<uint32_t> idx;
        const auto& files = fs->Files();
        for (uint32_t i = 0; i < files.size(); ++i)
            if (!files[i].IsProtected())
                idx.push_back(i);
        const size_t top = std::min<size_t>(50, idx.size());
        std::partial_sort(idx.begin(), idx.begin() + top, idx.end(), [&](uint32_t a, uint32_t b) { return files[a].size > files[b].size; });
        md += "## Top 50 largest files\n\n| # | Size | Modified | Type | Source | Path |\n|---:|---:|---|---|---|---|\n";
        for (size_t k = 0; k < top; ++k) {
            const auto& e = files[idx[k]];
            const bool saved = in.verified && in.verified->IsVerified(e.path, e.size, e.mtime);
            md += std::format("| {} | {} | {} | {} | {} | {}{} |\n", k + 1, FormatBytes(e.size), e.mtime > 0 ? Local(e.mtime, "{:%Y-%m-%d}") : "",
                CategoryName(e.category), SourceName(e.source), MdCell(DisplayPath(*fs, e)), saved ? " (saved on PC)" : "");
        }
        md += "\n";

        // ---- Top folders ----
        std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> byFolder; // bytes, count
        for (const auto& e : files) {
            if (e.IsProtected())
                continue;
            auto& f = byFolder[DisplayFolder(*fs, e)];
            f.first += e.size;
            ++f.second;
        }
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> folders(byFolder.begin(), byFolder.end());
        std::ranges::sort(folders, [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        md += "## Top folders by size\n\nFiles directly inside each folder (not counting subfolders).\n\n";
        md += "| Folder | Files | Size |\n|---|---:|---:|\n";
        for (size_t k = 0; k < std::min<size_t>(30, folders.size()); ++k)
            md += std::format("| {} | {} | {} |\n", MdCell(folders[k].first), Count(folders[k].second.second), FormatBytes(folders[k].second.first));
        md += "\n";
    }

    // ---- Apps ----
    if (in.apps) {
        size_t user = 0, sys = 0;
        for (const auto& a : *in.apps)
            (a.system ? sys : user)++;
        md += std::format("## Apps\n\n{} user apps, {} system apps. Full details for user apps are in `applist.txt`.\n\n", user, sys);
        md += "| Flag | Severity | User apps | Examples |\n|---|---|---:|---|\n";
        for (const auto& fi : AllFlags()) {
            std::vector<std::string> names;
            for (const auto& a : *in.apps)
                if (!a.system && a.Has(fi.flag))
                    names.push_back(a.package);
            if (names.empty())
                continue;
            std::string ex;
            for (size_t k = 0; k < std::min<size_t>(5, names.size()); ++k)
                ex += (k ? ", " : "") + MdCell(names[k]);
            if (names.size() > 5)
                ex += std::format(" (+{})", names.size() - 5);
            const char* sev = fi.severity == FlagSeverity::High ? "high" : fi.severity == FlagSeverity::Medium ? "medium" : "info";
            md += std::format("| {} | {} | {} | {} |\n", fi.name, sev, names.size(), ex);
        }
        md += "\n### Apps with high-risk access\n\n| App | Package | Installer | Flags |\n|---|---|---|---|\n";
        constexpr uint32_t high = AppFlag_Accessibility | AppFlag_DeviceAdmin | AppFlag_Sms | AppFlag_NotifListener | AppFlag_CallLog;
        size_t shown = 0;
        for (const auto& a : *in.apps) {
            if (a.system || !(a.flags & high))
                continue;
            std::string flags;
            for (const auto& fi : AllFlags())
                if (a.flags & fi.flag)
                    flags += (flags.empty() ? "" : ", ") + std::string(fi.name);
            md += std::format("| {} | {} | {} | {} |\n", MdCell(a.label.empty() ? "unknown" : a.label), MdCell(a.package), MdCell(InstallerToken(a)), flags);
            ++shown;
        }
        if (!shown)
            md += "| none | | | |\n";
        md += "\n";
    }

    // ---- Notes ----
    md += "## Scan notes\n\n";
    md += std::format("- Folders that couldn't be read (permission denied): {}\n", in.permissionDenied);
    md += std::format("- Files with broken names the phone can't open: {}\n", in.unreadableNames);
    if (in.verified)
        md += std::format("- Files saved and verified on the PC: {}\n", Count(in.verified->Size()));
    return md;
}

ExportResult ExportReports(const ReportInputs& in, const std::filesystem::path& reportsRoot, unsigned parts)
{
    ExportResult r;
    const int64_t now = in.now ? in.now : Now();
    r.dir = reportsRoot / Widen(Local(now, "{:%Y-%m-%d_%H%M%S}"));
    std::error_code ec;
    std::filesystem::create_directories(r.dir, ec);
    if (ec) {
        r.error = "Couldn't create " + Narrow(r.dir.wstring()) + ": " + ec.message();
        return r;
    }
    auto write = [&](const wchar_t* name, std::string_view data) {
        if (WriteFile(r.dir / name, data))
            r.files.push_back(Narrow(name));
        else
            r.error = "Couldn't write " + Narrow(name);
    };
    if (parts & Export_AppList)
        write(L"applist.txt", FormatUserAppList(in));
    if (parts & Export_ScanReport)
        write(L"scan_report.md", FormatScanReport(in));
    if ((parts & Export_Logs) && !in.sessionLog.empty()) {
        std::ifstream log(in.sessionLog, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        if (in.maskSerial && in.device.serial.size() > 4) {
            const std::string masked = MaskSerial(in.device.serial);
            for (size_t p; (p = text.find(in.device.serial)) != std::string::npos;)
                text.replace(p, in.device.serial.size(), masked);
        }
        std::filesystem::create_directories(r.dir / L"logs", ec);
        if (WriteFile(r.dir / L"logs" / in.sessionLog.filename(), text))
            r.files.push_back("logs/" + Narrow(in.sessionLog.filename().wstring()));
    }
    const nlohmann::json meta = { { "format", "phonesweep-export" }, { "created", Local(now, "{:%Y-%m-%d %H:%M:%S}") },
        { "device", in.device.displayName }, { "serial", in.maskSerial ? MaskSerial(in.device.serial) : in.device.serial }, { "files", r.files } };
    WriteFile(r.dir / L"export.json", meta.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
    r.ok = r.error.empty();
    LogInfo("Exported reports to {} ({} files)", Narrow(r.dir.wstring()), r.files.size());
    return r;
}

std::vector<ExportInfo> ListExports(const std::filesystem::path& root)
{
    std::vector<ExportInfo> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec))
            continue;
        ExportInfo info;
        info.dir = it->path();
        info.created = Narrow(it->path().filename().wstring());
        std::ifstream in(it->path() / L"export.json", std::ios::binary);
        if (!in)
            continue; // not ours
        try {
            const auto j = nlohmann::json::parse(in);
            if (j.value("format", "") != "phonesweep-export")
                continue;
            info.created = j.value("created", info.created);
            info.device = j.value("device", "");
            for (const auto& f : j.value("files", nlohmann::json::array()))
                info.files.push_back(f.get<std::string>());
        } catch (const nlohmann::json::exception&) {
            continue;
        }
        for (std::filesystem::recursive_directory_iterator f(it->path(), ec), fend; !ec && f != fend; f.increment(ec))
            if (f->is_regular_file(ec))
                info.bytes += f->file_size(ec);
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, [](const ExportInfo& a, const ExportInfo& b) { return a.created > b.created; });
    return out;
}

} // namespace ps
