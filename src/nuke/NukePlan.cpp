#include "nuke/NukePlan.h"

#include "scan/AppModel.h"
#include "scan/FileStore.h"
#include "transfer/Manifest.h"
#include "util/Strings.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace ps {

NukeDecision DecideNuke(const NukeFileFacts& f, ProtectorMode mode)
{
    // 1. The user's explicit "no" always wins.
    if (f.userExcluded)
        return NukeDecision::Excluded;
    // 2. Protected folders: never implicitly; in Free mode only when explicitly ticked.
    if (f.isProtected && !(mode == ProtectorMode::Free && f.userIncludedProtected))
        return NukeDecision::Protected;
    // 3. KEEP means "I want this": only deletable once it's safely on the PC.
    if (f.keep && !f.verified)
        return NukeDecision::KeepUnsaved;
    // 4. Nuke prompt: nothing leaves the phone unless it's saved + verified.
    if (mode == ProtectorMode::Prompt && !f.verified)
        return NukeDecision::NotSaved;
    return NukeDecision::Delete;
}

uint64_t NukePlan::Fingerprint() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(static_cast<uint64_t>(mode));
    mix(del.files.size());
    for (uint32_t i : del.files)
        mix(i);
    mix(uninstall.size());
    for (const auto& p : uninstall)
        for (char c : p)
            mix(static_cast<uint8_t>(c));
    return h;
}

NukePlan BuildNukePlan(const FileStore& files, const VerifiedIndex& verified, const std::vector<AppInfo>& apps,
    const std::set<std::string, std::less<>>& uninstallSet, const NukeOverrides& ov, ProtectorMode mode)
{
    NukePlan plan;
    plan.mode = mode;
    const auto& list = files.Files();
    for (uint32_t i = 0; i < list.size(); ++i) {
        const FileEntry& e = list[i];
        NukeFileFacts f;
        f.keep = (e.flags & FileFlag_Keep) != 0;
        f.verified = verified.IsVerified(e.path, e.size, e.mtime);
        f.isProtected = e.IsProtected();
        f.userExcluded = ov.excludedPaths.contains(e.path);
        f.userIncludedProtected = ov.includedProtectedPaths.contains(e.path);
        NukeGroup* g = nullptr;
        switch (DecideNuke(f, mode)) {
        case NukeDecision::Delete: g = &plan.del; break;
        case NukeDecision::KeepUnsaved: g = &plan.keepUnsaved; break;
        case NukeDecision::NotSaved: g = &plan.notSaved; break;
        case NukeDecision::Protected: g = &plan.protectedFiles; break;
        case NukeDecision::Excluded: g = &plan.excluded; break;
        }
        g->files.push_back(i);
        g->bytes += e.Disk();
    }
    for (const auto& a : apps)
        if (!a.system && uninstallSet.contains(a.package) && !ov.excludedApps.contains(a.package))
            plan.uninstall.push_back(a.package);
    return plan;
}

std::string FormatNukePlan(const NukePlan& plan, const FileStore& files, std::string_view deviceName)
{
    const auto now = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    std::string out;
    out += std::format("PhoneSweep nuke plan (dry run)\nDevice: {}\nCreated: {:%Y-%m-%d %H:%M:%S}\nProtector: {}\n\n", deviceName, now,
        plan.mode == ProtectorMode::Prompt ? "Nuke prompt (only saved + verified files)" : "Nuke free (everything in the plan)");
    out += std::format("DELETE {} files ({})\n", plan.del.files.size(), FormatBytes(plan.del.bytes));
    out += std::format("UNINSTALL {} apps\n", plan.uninstall.size());
    out += std::format("THEN {}\n", plan.clearCaches ? "clear all app caches (pm trim-caches)" : "no cache clearing");
    out += std::format("\nNot deleted: {} KEEP-but-unsaved, {} not saved (prompt), {} protected, {} unticked\n", plan.keepUnsaved.files.size(),
        plan.notSaved.files.size(), plan.protectedFiles.files.size(), plan.excluded.files.size());

    out += "\n== Files to delete ==\n";
    for (uint32_t i : plan.del.files) {
        const auto& e = files.Files()[i];
        out += std::format("{}\t{}\n", FormatBytes(e.size), e.path);
    }
    out += "\n== Apps to uninstall ==\n";
    for (const auto& p : plan.uninstall)
        out += p + "\n";
    if (!plan.keepUnsaved.files.empty()) {
        out += "\n== Marked KEEP but not saved (left on the phone) ==\n";
        for (uint32_t i : plan.keepUnsaved.files)
            out += files.Files()[i].path + "\n";
    }
    return out;
}

std::vector<std::string> CandidateEmptyDirs(const std::vector<std::string>& deletedPaths, const std::vector<std::string>& volumeRoots)
{
    std::set<std::string> dirs;
    for (const auto& p : deletedPaths) {
        // Find which volume the file is on.
        const std::string* root = nullptr;
        for (const auto& r : volumeRoots)
            if (p.starts_with(r))
                root = &r;
        if (!root)
            continue;
        std::string_view rel = std::string_view(p).substr(root->size());
        // Walk up the parents: a/b/c/file -> a/b/c, a/b (never "a": top level).
        size_t slash = rel.rfind('/');
        while (slash != std::string_view::npos) {
            const std::string_view dir = rel.substr(0, slash);
            const bool topLevel = dir.find('/') == std::string_view::npos;
            const bool android = dir.starts_with("Android/") || dir == "Android";
            const bool standard = dir == "DCIM/Camera" || dir == "DCIM/Screenshots" || dir == "Pictures/Screenshots";
            if (topLevel || android)
                break;
            if (!standard)
                dirs.insert(*root + std::string(dir));
            slash = dir.rfind('/');
        }
    }
    std::vector<std::string> out(dirs.begin(), dirs.end());
    // Deepest first so parents are tried after their children.
    std::ranges::stable_sort(out, [](const std::string& a, const std::string& b) {
        return std::ranges::count(a, '/') > std::ranges::count(b, '/');
    });
    return out;
}

std::string SqlQuote(std::string_view s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

} // namespace ps
