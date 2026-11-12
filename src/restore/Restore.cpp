#include "restore/Restore.h"

#include "adb/AdbClient.h"
#include "scan/ScanParse.h"
#include "backup/Zip.h"
#include "backup/ZipBackup.h"
#include "transfer/LocalPath.h"
#include "transfer/PhoneIO.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <memory>

namespace ps {

const char* ToString(RestoreState s)
{
    switch (s) {
    case RestoreState::Missing: return "missing on the phone";
    case RestoreState::Present: return "already on the phone";
    case RestoreState::Different: return "different file on the phone";
    case RestoreState::NoLocal: return "backup file missing on the PC";
    }
    return "?";
}

std::vector<BackupFolderInfo> ListBackupFolders(const std::filesystem::path& root)
{
    std::vector<BackupFolderInfo> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto file = it->path() / L"saved_manifest.json";
        std::ifstream in(file, std::ios::binary);
        if (!in)
            continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        BackupFolderInfo info;
        std::vector<ManifestEntry> entries;
        if (!Manifest::FromJson(text, &info.serial, &entries))
            continue;
        try {
            info.model = nlohmann::json::parse(text).value("model", "");
        } catch (const nlohmann::json::exception&) {
        }
        info.folder = it->path();
        info.files = entries.size();
        for (const auto& e : entries)
            info.bytes += e.size;
        out.push_back(std::move(info));
    }
    // Zipped backups (M9).
    ec.clear();
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != L".zip")
            continue;
        const auto zm = ReadZipManifest(it->path());
        if (!zm)
            continue;
        BackupFolderInfo info;
        info.folder = it->path();
        info.serial = zm->serial;
        info.model = zm->model;
        info.files = zm->entries.size();
        for (const auto& e : zm->entries)
            info.bytes += e.size;
        info.zip = true;
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, [](const BackupFolderInfo& a, const BackupFolderInfo& b) { return a.folder.filename() > b.folder.filename(); });
    return out;
}

std::string MapRestoreTarget(std::string_view devicePath, const std::vector<std::string>& volumeRoots)
{
    constexpr std::string_view kInternal = "/storage/emulated/0/";
    if (devicePath.starts_with(kInternal) || !devicePath.starts_with("/storage/"))
        return std::string(devicePath);
    // /storage/XXXX-XXXX/rest
    const auto slash = devicePath.find('/', 9);
    if (slash == std::string_view::npos)
        return std::string(devicePath);
    const std::string_view rest = devicePath.substr(slash + 1);
    for (const auto& r : volumeRoots)
        if (r != kInternal && r.starts_with("/storage/"))
            return r + std::string(rest); // this phone's SD card
    return std::string(kInternal) + "Restored from SD card/" + std::string(rest);
}

std::vector<std::string> PhoneVolumeRoots(const AdbClient& adb, const std::string& serial, std::stop_token st)
{
    std::vector<std::string> roots = { "/storage/emulated/0/" };
    const auto r = adb.Shell(serial, ShellCommand("ls").arg("-1").arg("/storage"), { .timeout = 20s, .cancel = st });
    for (const auto& id : ParseStorageVolumes(r.out))
        roots.push_back("/storage/" + id + "/");
    return roots;
}

std::vector<RestoreItem> PlanRestore(const AdbClient& adb, const std::string& serial, const std::filesystem::path& folder,
    const std::vector<std::string>& volumeRoots, std::stop_token st)
{
    std::vector<RestoreItem> items;
    std::error_code ec;
    if (folder.extension() == L".zip") {
        ZipReader zr;
        std::string err;
        if (zr.Open(folder, &err))
            if (const auto* me = zr.Find(kManifestName))
                if (const auto text = zr.ReadAll(*me, &err)) {
                    std::string zipSerial;
                    std::vector<ManifestEntry> entries;
                    Manifest::FromJson(*text, &zipSerial, &entries);
                    for (auto& e : entries) {
                        RestoreItem it;
                        it.local = folder;
                        it.zipEntry = e.localRel;
                        it.target = MapRestoreTarget(e.devicePath, volumeRoots);
                        it.state = zr.Find(e.localRel) ? RestoreState::Missing : RestoreState::NoLocal;
                        it.entry = std::move(e);
                        items.push_back(std::move(it));
                    }
                }
    } else {
        Manifest m(folder / L"saved_manifest.json", "", "");
        m.Load();
        for (auto& e : m.Entries()) {
            RestoreItem it;
            it.local = folder / std::filesystem::path(Widen(e.localRel));
            it.target = MapRestoreTarget(e.devicePath, volumeRoots);
            it.state = std::filesystem::is_regular_file(LongPath(it.local), ec) ? RestoreState::Missing : RestoreState::NoLocal;
            it.entry = std::move(e);
            items.push_back(std::move(it));
        }
    }
    // What's on the phone already? One line per file: size, or "-".
    for (size_t i = 0; i < items.size() && !st.stop_requested(); i += 100) {
        const size_t n = std::min<size_t>(100, items.size() - i);
        ShellCommand cmd("for f in");
        for (size_t k = 0; k < n; ++k)
            cmd.arg(items[i + k].target);
        cmd.raw("; do [ -e \"$f\" ] && stat -c %s \"$f\" || echo -; done");
        const auto r = adb.Shell(serial, cmd, { .timeout = 120s, .cancel = st });
        std::vector<std::string> lines;
        ForEachLine(r.out, [&](std::string_view l) {
            l = Trim(l);
            if (!l.empty())
                lines.emplace_back(l);
        });
        if (lines.size() != n)
            continue; // can't tell: leave as Missing (restore will re-check per file)
        for (size_t k = 0; k < n; ++k) {
            auto& it = items[i + k];
            if (it.state == RestoreState::NoLocal || lines[k] == "-")
                continue;
            const auto size = ParseInt<uint64_t>(lines[k]);
            it.state = size && *size == it.entry.size ? RestoreState::Present : RestoreState::Different;
        }
    }
    return items;
}

void RunRestore(const AdbClient& adb, const std::string& serial, const std::vector<RestoreItem>& items, RestorePolicy policy,
    RestoreProgress& p, std::stop_token st)
{
    {
        std::lock_guard l(p.mutex);
        p.total = items.size();
    }
    std::vector<std::string> restored;
    auto fail = [&](const RestoreItem& it, const std::string& why) {
        LogWarn("Restore failed for {}: {}", it.target, why);
        std::lock_guard l(p.mutex);
        ++p.failed;
        if (p.errors.size() < 10)
            p.errors.push_back(it.target.substr(it.target.rfind('/') + 1) + ": " + why);
    };
    // Zipped backups: one reader per zip, one temp file reused per item.
    std::unique_ptr<ZipReader> zip;
    std::filesystem::path zipPath;
    const auto tmpDir = std::filesystem::temp_directory_path() / L"PhoneSweep_restore";
    std::error_code ec;
    for (const auto& it : items) {
        if (st.stop_requested())
            break;
        {
            std::lock_guard l(p.mutex);
            p.current = it.target.substr(it.target.rfind('/') + 1);
        }
        std::string target = it.target;
        if (it.state == RestoreState::NoLocal) {
            fail(it, "the backup copy is missing on the PC");
        } else if ((it.state == RestoreState::Present || it.state == RestoreState::Different) && policy == RestorePolicy::SkipExisting) {
            std::lock_guard l(p.mutex);
            ++p.skipped;
        } else {
            if ((it.state == RestoreState::Present || it.state == RestoreState::Different) && policy == RestorePolicy::KeepBoth) {
                const auto dot = target.rfind('.');
                const auto slash = target.rfind('/');
                target = (dot != std::string::npos && dot > slash) ? target.substr(0, dot) + " (restored)" + target.substr(dot) : target + " (restored)";
            }
            // Zipped backup: extract this one file to a temp copy first (CRC-checked).
            std::filesystem::path source = it.local;
            std::string zipErr;
            bool extracted = true;
            if (!it.zipEntry.empty()) {
                if (!zip || zipPath != it.local) {
                    zip = std::make_unique<ZipReader>();
                    zipPath = it.local;
                    if (!zip->Open(it.local, &zipErr))
                        zip.reset();
                }
                const ZipEntry* ze = zip ? zip->Find(it.zipEntry) : nullptr;
                std::filesystem::create_directories(tmpDir, ec);
                source = tmpDir / L"restore_item";
                extracted = ze && zip->Extract(*ze, source, st, &zipErr);
            }
            // The PC copy must still be the file we verified when saving it.
            const auto md5 = extracted ? Md5File(LongPath(source)) : std::nullopt;
            if (!extracted) {
                fail(it, "couldn't read it from the zip: " + (zipErr.empty() ? std::string("missing") : zipErr));
            } else if (!md5 || (!it.entry.md5.empty() && *md5 != it.entry.md5)) {
                fail(it, "the backup copy on the PC no longer matches its checksum");
            } else {
                const std::string err = PushVerified(adb, serial, source, target, *md5, it.entry.mtime, st);
                if (err.empty()) {
                    restored.push_back(target);
                    LogInfo("Restored {}", target);
                    std::lock_guard l(p.mutex);
                    ++p.restored;
                    p.bytes += it.entry.size;
                } else {
                    fail(it, err);
                }
            }
        }
        std::lock_guard l(p.mutex);
        ++p.done;
    }
    {
        std::lock_guard l(p.mutex);
        p.current = "updating the gallery";
    }
    std::filesystem::remove_all(tmpDir, ec);
    MediaScan(adb, serial, restored, st);
    std::lock_guard l(p.mutex);
    p.finished = true;
    LogInfo("Restore finished: {} restored, {} skipped, {} failed", p.restored, p.skipped, p.failed);
}

} // namespace ps
