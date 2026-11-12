#include "transfer/Manifest.h"

#include "backup/ZipBackup.h"

#include "util/Log.h"
#include "util/Utf8.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace ps {

namespace {

std::string ToHex(std::string_view s)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

std::optional<std::string> FromHex(std::string_view h)
{
    if (h.size() % 2)
        return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < h.size(); i += 2) {
        const int a = nib(h[i]), b = nib(h[i + 1]);
        if (a < 0 || b < 0)
            return std::nullopt;
        out.push_back(static_cast<char>((a << 4) | b));
    }
    return out;
}

} // namespace

bool IsValidUtf8(std::string_view s)
{
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size() || (n == 2 && c < 0xC2))
            return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += n;
    }
    return true;
}

Manifest::Manifest(std::filesystem::path file, std::string serial, std::string model)
    : m_file(std::move(file))
    , m_serial(std::move(serial))
    , m_model(std::move(model))
{
}

std::string Manifest::ToJson(const std::string& serial, const std::string& model, const std::vector<ManifestEntry>& entries)
{
    nlohmann::json files = nlohmann::json::array();
    for (const auto& e : entries) {
        nlohmann::json j = { { "size", e.size }, { "mtime", e.mtime }, { "verified", e.Method() }, { "verifiedAt", e.verifiedAt },
            { "kind", e.kind } };
        // Raw device bytes must round-trip exactly: hex when they aren't valid UTF-8.
        if (IsValidUtf8(e.devicePath))
            j["devicePath"] = e.devicePath;
        else
            j["devicePathHex"] = ToHex(e.devicePath);
        if (!e.md5.empty())
            j["md5"] = e.md5;
        j["local"] = e.localRel; // already sanitized UTF-8
        files.push_back(std::move(j));
    }
    const nlohmann::json root = { { "format", "phonesweep-manifest" }, { "version", 1 }, { "device", serial }, { "model", model },
        { "files", files } };
    return root.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool Manifest::FromJson(std::string_view json, std::string* serial, std::vector<ManifestEntry>* entries)
{
    try {
        const auto root = nlohmann::json::parse(json);
        if (root.value("format", "") != "phonesweep-manifest")
            return false;
        if (serial)
            *serial = root.value("device", "");
        if (entries) {
            for (const auto& j : root.value("files", nlohmann::json::array())) {
                ManifestEntry e;
                if (j.contains("devicePath"))
                    e.devicePath = j["devicePath"].get<std::string>();
                else if (auto raw = FromHex(j.value("devicePathHex", "")))
                    e.devicePath = *raw;
                else
                    continue;
                e.size = j.value("size", uint64_t{ 0 });
                e.mtime = j.value("mtime", int64_t{ 0 });
                e.md5 = j.value("md5", "");
                e.localRel = j.value("local", "");
                e.verifiedAt = j.value("verifiedAt", "");
                e.kind = j.value("kind", "file");
                if (!e.devicePath.empty())
                    entries->push_back(std::move(e));
            }
        }
        return true;
    } catch (const nlohmann::json::exception& ex) {
        LogWarn("Manifest unreadable: {}", ex.what());
        return false;
    }
}

bool Manifest::Load()
{
    std::ifstream in(m_file, std::ios::binary);
    if (!in)
        return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<ManifestEntry> entries;
    std::string serial;
    if (!FromJson(text, &serial, &entries))
        return false;
    std::lock_guard l(m_mutex);
    for (auto& e : entries)
        m_entries[e.devicePath] = std::move(e);
    return true;
}

void Manifest::Upsert(ManifestEntry e)
{
    std::lock_guard l(m_mutex);
    m_entries[e.devicePath] = std::move(e);
}

std::vector<ManifestEntry> Manifest::Entries() const
{
    std::lock_guard l(m_mutex);
    std::vector<ManifestEntry> out;
    out.reserve(m_entries.size());
    for (const auto& [k, v] : m_entries)
        out.push_back(v);
    return out;
}

size_t Manifest::Size() const
{
    std::lock_guard l(m_mutex);
    return m_entries.size();
}

bool Manifest::Save() const
{
    const std::string json = ToJson(m_serial, m_model, Entries());
    std::error_code ec;
    std::filesystem::create_directories(m_file.parent_path(), ec);
    auto tmp = m_file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out)
            return false;
    }
    std::filesystem::rename(tmp, m_file, ec); // atomic replace on NTFS
    return !ec;
}

// ---- VerifiedIndex ----------------------------------------------------------------

void VerifiedIndex::Clear()
{
    m_entries.clear();
    ++m_version;
}

size_t VerifiedIndex::LoadForDevice(const std::filesystem::path& root, std::string_view serial)
{
    Clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == L".zip") {
            // Zipped backup (M9): files inside still count as saved.
            if (const auto zm = ReadZipManifest(it->path()); zm && zm->serial == serial)
                for (const auto& e : zm->entries)
                    Add(e, it->path());
            continue;
        }
        if (!it->is_directory(ec))
            continue;
        const auto file = it->path() / L"saved_manifest.json";
        std::ifstream in(file, std::ios::binary);
        if (!in)
            continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string dev;
        std::vector<ManifestEntry> entries;
        if (!Manifest::FromJson(text, &dev, &entries) || dev != serial)
            continue;
        for (const auto& e : entries)
            Add(e, it->path());
    }
    LogInfo("Verified backups found for this phone: {} files", m_entries.size());
    return m_entries.size();
}

void VerifiedIndex::Add(const ManifestEntry& e, const std::filesystem::path& folder)
{
    // Prefer the newest verification (e.g. a later backup of an edited file).
    auto it = m_entries.find(e.devicePath);
    if (it == m_entries.end() || it->second.entry.verifiedAt <= e.verifiedAt)
        m_entries[e.devicePath] = { e, folder };
    ++m_version;
}

bool VerifiedIndex::IsVerified(std::string_view devicePath, uint64_t size, int64_t mtime) const
{
    auto it = m_entries.find(devicePath);
    if (it == m_entries.end())
        return false;
    const auto& e = it->second.entry;
    return e.size == size && e.mtime == mtime;
}

std::optional<ManifestEntry> VerifiedIndex::Find(std::string_view devicePath) const
{
    auto it = m_entries.find(devicePath);
    if (it == m_entries.end())
        return std::nullopt;
    return it->second.entry;
}

std::optional<std::filesystem::path> VerifiedIndex::LocalFile(std::string_view devicePath) const
{
    auto it = m_entries.find(devicePath);
    if (it == m_entries.end())
        return std::nullopt;
    if (it->second.folder.extension() == L".zip")
        return it->second.folder; // inside a zipped backup
    return it->second.folder / std::filesystem::path(Widen(it->second.entry.localRel));
}

} // namespace ps
