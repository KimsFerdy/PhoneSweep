#include "cleanup/Cleanup.h"

#include "adb/AdbClient.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
#include "util/Log.h"
#include "util/Strings.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace ps {

namespace {

constexpr size_t kHashBatch = 100;
constexpr uint64_t kPartialBytes = 65536;

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool IStartsWith(std::string_view s, std::string_view p)
{
    if (s.size() < p.size())
        return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (Lower(s[i]) != Lower(p[i]))
            return false;
    return true;
}

bool IContains(std::string_view s, std::string_view n)
{
    for (size_t i = 0; i + n.size() <= s.size(); ++i)
        if (IStartsWith(s.substr(i), n))
            return true;
    return false;
}

std::string Ext(std::string_view name)
{
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    std::string e(name.substr(dot + 1));
    for (char& c : e)
        c = Lower(c);
    return e;
}

bool IsMessaging(Source s) { return s == Source::WhatsApp || s == Source::WhatsAppBusiness || s == Source::Telegram || s == Source::Snapchat; }

// "Android/data/<pkg>/..." -> pkg
std::string_view PackageAfter(std::string_view rel, std::string_view prefix)
{
    if (!IStartsWith(rel, prefix))
        return {};
    auto rest = rel.substr(prefix.size());
    return rest.substr(0, rest.find('/'));
}

// msgstore-2026-11-10.1.db.crypt14 (dated daily backup; the undated msgstore.db.crypt* is the newest).
bool IsDatedChatBackup(std::string_view name)
{
    if (!name.starts_with("msgstore-") || name.starts_with("msgstore-increment"))
        return false;
    // msgstore-YYYY-MM-DD
    if (name.size() < 19)
        return false;
    for (size_t i : { 9u, 10u, 11u, 12u, 14u, 15u, 17u, 18u })
        if (name[i] < '0' || name[i] > '9')
            return false;
    return name[13] == '-' && name[16] == '-' && name.find(".db.crypt") != std::string_view::npos;
}

} // namespace

// ---- Duplicates ----------------------------------------------------------------

std::vector<std::vector<uint32_t>> SizeCandidates(const FileStore& files, uint64_t minSize)
{
    std::unordered_map<uint64_t, std::vector<uint32_t>> bySize;
    const auto& f = files.Files();
    for (uint32_t i = 0; i < f.size(); ++i)
        if (!f[i].IsProtected() && f[i].size >= std::max<uint64_t>(minSize, 1))
            bySize[f[i].size].push_back(i);
    std::vector<std::vector<uint32_t>> out;
    for (auto& [size, idx] : bySize)
        if (idx.size() >= 2)
            out.push_back(std::move(idx));
    // Biggest potential savings first.
    std::ranges::sort(out, [&](const auto& a, const auto& b) { return f[a[0]].size * (a.size() - 1) > f[b[0]].size * (b.size() - 1); });
    return out;
}

size_t ChooseKeeper(const std::vector<uint32_t>& group, const FileStore& files)
{
    const auto& f = files.Files();
    auto score = [&](uint32_t i) {
        const FileEntry& e = f[i];
        // Lexicographic preference tuple (bigger = better keeper).
        return std::tuple{ (e.flags & FileFlag_Keep) ? 1 : 0, e.source == Source::Camera ? 1 : 0, IsMessaging(e.source) ? 0 : 1,
            IContains(e.RelPath(), "/Sent/") ? 0 : 1, e.volume == 0 ? 1 : 0, -e.mtime, -static_cast<int64_t>(e.path.size()) };
    };
    size_t best = 0;
    for (size_t k = 1; k < group.size(); ++k)
        if (score(group[k]) > score(group[best]))
            best = k;
    return best;
}

std::vector<std::string> DuplicateDeleteList(const std::vector<DupGroup>& groups, const std::vector<bool>& ticked, const FileStore& files)
{
    std::unordered_map<std::string_view, const FileEntry*> byPath;
    for (const auto& e : files.Files())
        byPath.emplace(e.path, &e);
    std::vector<std::string> out;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g >= ticked.size() || !ticked[g])
            continue;
        const auto& grp = groups[g];
        if (grp.files.size() < 2 || grp.keep >= grp.files.size())
            continue;
        for (size_t k = 0; k < grp.files.size(); ++k) {
            if (k == grp.keep)
                continue;
            const auto it = byPath.find(grp.files[k].path);
            if (it == byPath.end())
                continue; // no longer on the phone (already deleted)
            if (it->second->flags & FileFlag_Keep)
                continue; // KEEP is never deleted
            out.push_back(grp.files[k].path);
        }
    }
    return out;
}

std::vector<std::string> HashOnPhone(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths, bool partial,
    std::stop_token st, const std::function<void(size_t)>& onProgress)
{
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size() && !st.stop_requested(); i += kHashBatch) {
        const size_t n = std::min(kHashBatch, paths.size() - i);
        // One output line per file, in order: the hash, or "-" when unreadable.
        ShellCommand cmd("for f in");
        for (size_t k = 0; k < n; ++k)
            cmd.arg(paths[i + k]);
        cmd.raw(partial ? "; do [ -r \"$f\" ] && h=$(head -c 65536 \"$f\" | md5sum) && echo \"${h%% *}\" || echo -; done"
                        : "; do [ -r \"$f\" ] && h=$(md5sum \"$f\" 2>/dev/null) && echo \"${h%% *}\" || echo -; done");
        const auto r = adb.Shell(serial, cmd, { .timeout = partial ? 120s : 60min, .cancel = st });
        std::vector<std::string> lines;
        ForEachLine(r.out, [&](std::string_view l) {
            l = Trim(l);
            if (!l.empty())
                lines.emplace_back(l);
        });
        // Misaligned output (odd names, errors) -> mark the whole batch unreadable; never guess.
        if (lines.size() != n)
            lines.assign(n, "-");
        out.insert(out.end(), lines.begin(), lines.end());
        if (onProgress)
            onProgress(out.size());
    }
    out.resize(paths.size(), "-");
    return out;
}

std::vector<DupGroup> FindDuplicates(const AdbClient& adb, const std::string& serial, const FileStore& snapshot, uint64_t minSize,
    std::stop_token st, const std::function<void(const DupProgress&)>& onProgress)
{
    const auto& f = snapshot.Files();
    const auto candidates = SizeCandidates(snapshot, minSize);

    // Stage 1: hash the first 64 KB of every candidate.
    std::vector<uint32_t> flat;
    for (const auto& g : candidates)
        flat.insert(flat.end(), g.begin(), g.end());
    std::vector<std::string> paths;
    for (uint32_t i : flat)
        paths.push_back(f[i].path);
    LogInfo("Duplicates: {} same-size groups, {} files to fingerprint", candidates.size(), flat.size());
    auto report = [&](const char* phase, size_t done, size_t total) {
        if (onProgress)
            onProgress({ phase, done, total });
    };
    report("Fingerprinting (first 64 KB)", 0, paths.size());
    const auto partial = HashOnPhone(adb, serial, paths, true, st,
        [&](size_t d) { report("Fingerprinting (first 64 KB)", d, paths.size()); });
    if (st.stop_requested())
        return {};

    // Regroup by (size, partial hash).
    std::map<std::pair<uint64_t, std::string>, std::vector<uint32_t>> stage2;
    for (size_t k = 0; k < flat.size(); ++k)
        if (partial[k] != "-")
            stage2[{ f[flat[k]].size, partial[k] }].push_back(flat[k]);

    // Stage 2: full MD5 for groups still matching (files <= 64 KB are already fully hashed).
    std::vector<uint32_t> needFull;
    for (const auto& [key, idx] : stage2)
        if (idx.size() >= 2 && key.first > kPartialBytes)
            needFull.insert(needFull.end(), idx.begin(), idx.end());
    std::vector<std::string> fullPaths;
    for (uint32_t i : needFull)
        fullPaths.push_back(f[i].path);
    report("Confirming with full checksums", 0, fullPaths.size());
    const auto full = HashOnPhone(adb, serial, fullPaths, false, st,
        [&](size_t d) { report("Confirming with full checksums", d, fullPaths.size()); });
    if (st.stop_requested())
        return {};
    std::unordered_map<uint32_t, std::string> fullOf;
    for (size_t k = 0; k < needFull.size(); ++k)
        fullOf[needFull[k]] = full[k];

    std::map<std::pair<uint64_t, std::string>, std::vector<uint32_t>> finalGroups;
    for (const auto& [key, idx] : stage2) {
        if (idx.size() < 2)
            continue;
        for (uint32_t i : idx) {
            const std::string h = key.first <= kPartialBytes ? key.second : fullOf[i];
            if (h != "-")
                finalGroups[{ key.first, h }].push_back(i);
        }
    }

    std::vector<DupGroup> out;
    for (const auto& [key, idx] : finalGroups) {
        if (idx.size() < 2)
            continue;
        DupGroup g;
        g.size = key.first;
        g.md5 = key.second;
        for (uint32_t i : idx)
            g.files.push_back({ f[i].path, f[i].size, f[i].mtime });
        g.keep = ChooseKeeper(idx, snapshot);
        out.push_back(std::move(g));
    }
    std::ranges::sort(out, [](const DupGroup& a, const DupGroup& b) { return a.Reclaimable() > b.Reclaimable(); });
    uint64_t total = 0;
    for (const auto& g : out)
        total += g.Reclaimable();
    LogInfo("Duplicates: {} confirmed groups, {} bytes reclaimable", out.size(), total);
    return out;
}

// ---- Junk ------------------------------------------------------------------------

const JunkInfo& GetJunkInfo(JunkKind k)
{
    static const std::array<JunkInfo, kJunkKindCount> kInfo = { {
        { JunkKind::Thumbnails, "Thumbnail caches", "Gallery previews in .thumbnails folders. Android rebuilds them when needed. Samsung's .thumbdata "
          "files show several GB in file managers but are sparse: sizes here are the real space they use.", true },
        { JunkKind::AppCaches, "App caches", "Files in Android/data/<app>/cache. Apps re-download or rebuild them.", true },
        { JunkKind::Leftovers, "Leftovers of uninstalled apps", "Android/data and Android/obb folders of apps that are no longer installed.", true },
        { JunkKind::WhatsAppStatuses, "Viewed WhatsApp statuses", "Cached copies of statuses you already watched (.Statuses). They expire after 24 h anyway.", true },
        { JunkKind::LogsTemp, "Logs and temp files", "*.log, *.tmp and .cache folders left by apps.", true },
        { JunkKind::OldChatBackups, "Old WhatsApp chat backups", "Dated daily backups (msgstore-YYYY-MM-DD). The newest backup is always kept.", false },
        { JunkKind::Installers, "APK installers", "Downloaded .apk/.xapk files. Usually already installed, but you may want to keep some.", false },
        { JunkKind::EmptyFiles, "Empty files", "0-byte files (hidden marker files like .nomedia are never included).", false },
    } };
    return kInfo[static_cast<size_t>(k)];
}

std::optional<JunkKind> ClassifyJunk(const FileEntry& e, const std::set<std::string, std::less<>>& installed)
{
    const std::string_view rel = e.RelPath();
    const std::string_view name = e.Name();
    if (IContains(rel, "/.thumbnails/") || IStartsWith(rel, ".thumbnails/"))
        return JunkKind::Thumbnails;
    for (const char* root : { "Android/data/", "Android/obb/" }) {
        const auto pkg = PackageAfter(rel, root);
        if (pkg.empty() || rel.size() <= std::string_view(root).size() + pkg.size())
            continue;
        if (!installed.empty() && !installed.contains(pkg))
            return JunkKind::Leftovers;
        if (std::string_view(root) == "Android/data/" && IStartsWith(rel.substr(std::string_view(root).size() + pkg.size()), "/cache/"))
            return JunkKind::AppCaches;
        return std::nullopt; // an installed app's own data: not junk
    }
    if (IContains(rel, "/.Statuses/"))
        return JunkKind::WhatsAppStatuses;
    if (IContains(rel, "/.cache/") || IStartsWith(rel, ".cache/"))
        return JunkKind::LogsTemp;
    const std::string ext = Ext(name);
    if (ext == "log" || ext == "tmp" || ext == "temp")
        return JunkKind::LogsTemp;
    if (IContains(rel, "/Databases/") && IsDatedChatBackup(name))
        return JunkKind::OldChatBackups;
    if (ext == "apk" || ext == "xapk" || ext == "apks" || ext == "apkm")
        return JunkKind::Installers;
    if (e.size == 0 && !name.starts_with("."))
        return JunkKind::EmptyFiles;
    return std::nullopt;
}

std::vector<JunkCategory> BuildJunk(const FileStore& files, const VerifiedIndex& verified, const std::set<std::string, std::less<>>& installed,
    ProtectorMode mode)
{
    std::vector<JunkCategory> cats(kJunkKindCount);
    for (size_t k = 0; k < kJunkKindCount; ++k)
        cats[k].kind = static_cast<JunkKind>(k);
    const auto& f = files.Files();
    for (uint32_t i = 0; i < f.size(); ++i) {
        const FileEntry& e = f[i];
        if (e.flags & FileFlag_Keep)
            continue; // KEEP is never junk
        const auto kind = ClassifyJunk(e, installed);
        if (!kind)
            continue;
        auto& c = cats[static_cast<size_t>(*kind)];
        const bool safe = GetJunkInfo(*kind).safe;
        // Nuke prompt protects Review junk unless it's saved on the PC.
        if (!safe && mode == ProtectorMode::Prompt && !verified.IsVerified(e.path, e.size, e.mtime)) {
            c.blocked.push_back(i);
            c.blockedBytes += e.Disk();
        } else {
            c.files.push_back(i);
            c.bytes += e.Disk();
        }
    }
    return cats;
}

} // namespace ps
