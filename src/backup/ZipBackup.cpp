#include "backup/ZipBackup.h"

#include "backup/Zip.h"
#include "transfer/LocalPath.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <map>

namespace ps {

std::filesystem::path ZipPathFor(const std::filesystem::path& folder)
{
    auto zip = folder;
    zip += L".zip";
    return NextFreeName(zip);
}

std::optional<ZipManifest> ReadZipManifest(const std::filesystem::path& zip)
{
    ZipReader r;
    std::string err;
    if (!r.Open(zip, &err))
        return std::nullopt;
    const auto* e = r.Find(kManifestName);
    if (!e)
        return std::nullopt;
    const auto text = r.ReadAll(*e, &err);
    if (!text)
        return std::nullopt;
    ZipManifest m;
    if (!Manifest::FromJson(*text, &m.serial, &m.entries))
        return std::nullopt;
    try {
        m.model = nlohmann::json::parse(*text).value("model", "");
    } catch (const nlohmann::json::exception&) {
    }
    return m;
}

void ZipBackupFolder(const std::filesystem::path& folder, bool deleteFolder, ZipProgress& p, std::stop_token st)
{
    auto finish = [&](bool ok, std::string error) {
        if (!ok)
            LogWarn("Zip of {} failed: {}", Narrow(folder.wstring()), error);
        std::lock_guard l(p.mutex);
        p.ok = ok;
        p.error = std::move(error);
        p.finished = true;
    };

    // 1. What goes in: every file, manifest first so tools (and we) find it fast.
    struct Item {
        std::string name; // '/'-separated, UTF-8
        std::filesystem::path path;
        uint64_t size;
        int64_t mtime;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(LongPath(folder), ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const auto rel = std::filesystem::relative(it->path(), LongPath(folder), ec);
        if (ec)
            break;
        Item item;
        item.name = Narrow(rel.generic_wstring());
        item.path = it->path();
        item.size = it->file_size(ec);
        const auto ft = it->last_write_time(ec);
        item.mtime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch()).count();
        total += item.size;
        items.push_back(std::move(item));
    }
    if (ec) {
        finish(false, "couldn't list the folder: " + ec.message());
        return;
    }
    const auto manifestIt = std::ranges::find(items, std::string(kManifestName), &Item::name);
    if (manifestIt == items.end()) {
        finish(false, "this folder has no saved_manifest.json, so it isn't a PhoneSweep backup");
        return;
    }
    std::rotate(items.begin(), manifestIt, manifestIt + 1);
    std::sort(items.begin() + 1, items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });

    const auto zip = ZipPathFor(folder);
    auto part = zip;
    part += L".part";
    {
        std::lock_guard l(p.mutex);
        p.phase = "Zipping";
        p.total = total * 2; // write + verify read
        p.zip = zip;
        p.files = items.size();
    }
    if (const uint64_t space = FreeSpace(folder.parent_path()); space > 0 && space < total + (64ull << 20)) {
        finish(false, "not enough free space on the PC for the zip");
        return;
    }
    LogInfo("Zipping {} ({} files, {} bytes) -> {}", Narrow(folder.wstring()), items.size(), total, Narrow(zip.wstring()));

    // 2. Write.
    std::string err;
    {
        ZipWriter w;
        bool ok = w.Open(part, &err);
        for (size_t i = 0; ok && i < items.size(); ++i) {
            {
                std::lock_guard l(p.mutex);
                p.current = items[i].name;
            }
            ok = w.AddFile(items[i].name, items[i].path, items[i].mtime, st, [&](uint64_t n) {
                std::lock_guard l(p.mutex);
                p.done += n;
            }, &err);
        }
        ok = ok && w.Close(&err);
        if (!ok) {
            std::filesystem::remove(LongPath(part), ec);
            finish(false, err);
            return;
        }
    }

    // 3. Verify: re-read everything; manifest files must match their MD5.
    {
        std::lock_guard l(p.mutex);
        p.phase = "Verifying";
    }
    bool verified = false;
    {
        ZipReader r;
        verified = r.Open(part, &err);
        if (verified && r.Entries().size() != items.size()) {
            verified = false;
            err = "the zip has the wrong number of files";
        }
        std::map<std::string, std::string, std::less<>> expectedMd5;
        if (verified) {
            for (const auto& e : r.Entries())
                if (e.name == kManifestName)
                    if (const auto text = r.ReadAll(e, &err)) {
                        std::string serial;
                        std::vector<ManifestEntry> entries;
                        if (Manifest::FromJson(*text, &serial, &entries))
                            for (const auto& m : entries)
                                expectedMd5[m.localRel] = m.md5;
                    }
        }
        for (size_t i = 0; verified && i < r.Entries().size(); ++i) {
            const auto& e = r.Entries()[i];
            {
                std::lock_guard l(p.mutex);
                p.current = e.name;
            }
            Md5 md5;
            verified = r.Read(e, [&](std::span<const uint8_t> d) {
                md5.Update(d);
                std::lock_guard l(p.mutex);
                p.done += d.size();
            }, st, &err);
            if (!verified)
                break;
            const std::string got = md5.HexFinal();
            if (auto it = expectedMd5.find(e.name); it != expectedMd5.end() && !it->second.empty() && it->second != got) {
                verified = false;
                err = e.name + " doesn't match the MD5 recorded when it was saved";
            }
            if (e.size != items[i].size) {
                verified = false;
                err = e.name + " has the wrong size in the zip";
            }
        }
    }
    if (!verified) {
        std::filesystem::remove(LongPath(part), ec);
        finish(false, err.empty() ? std::string("verification failed") : err);
        return;
    }
    std::filesystem::rename(LongPath(part), LongPath(zip), ec);
    if (ec) {
        std::filesystem::remove(LongPath(part), ec);
        finish(false, "couldn't rename the finished zip");
        return;
    }
    LogInfo("Zip verified: {} ({} files)", Narrow(zip.wstring()), items.size());

    // 4. Optionally remove the loose folder: the verified zip replaces it.
    if (deleteFolder && !st.stop_requested()) {
        {
            std::lock_guard l(p.mutex);
            p.phase = "Removing the folder";
        }
        std::filesystem::remove_all(LongPath(folder), ec);
        std::lock_guard l(p.mutex);
        p.folderRemoved = !ec;
        if (ec)
            LogWarn("Zip OK but couldn't remove {}: {}", Narrow(folder.wstring()), ec.message());
        else
            LogInfo("Removed {} (replaced by its verified zip)", Narrow(folder.wstring()));
    }
    finish(true, {});
}

} // namespace ps
