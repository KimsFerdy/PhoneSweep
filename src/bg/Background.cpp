#include "bg/Background.h"

#include "adb/AdbClient.h"
#include "util/Log.h"
#include "util/Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace ps {

namespace {

constexpr int kBucketActive = 10, kBucketRare = 40, kBucketRestricted = 45;

std::string NowIso()
{
    const auto now = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return std::format("{:%Y-%m-%d %H:%M:%S}", now);
}

} // namespace

std::map<std::string, BgState, std::less<>> ParseBgBatch(std::string_view text)
{
    std::map<std::string, BgState, std::less<>> out;
    BgState* cur = nullptr;
    ForEachLine(text, [&](std::string_view line) {
        const auto t = Trim(line);
        if (t.starts_with("@@")) {
            cur = &out[std::string(t.substr(2))];
            return;
        }
        if (!cur || t.empty())
            return;
        // "RUN_ANY_IN_BACKGROUND: ignore; time=+1m" (explicit) or "Default mode: allow".
        if (t.starts_with("RUN_ANY_IN_BACKGROUND:")) {
            auto v = Trim(t.substr(22));
            v = v.substr(0, v.find(';'));
            cur->mode = std::string(Trim(v));
            cur->explicitMode = true;
        } else if (t.starts_with("Default mode:") && !cur->explicitMode) {
            cur->mode = std::string(Trim(t.substr(13)));
        } else if (auto b = ParseInt<int>(t)) {
            cur->bucket = *b;
        }
    });
    return out;
}

const char* BucketName(int b)
{
    switch (b) {
    case 5: return "Exempted";
    case 10: return "Active";
    case 20: return "Working set";
    case 30: return "Frequent";
    case 40: return "Rare";
    case 45: return "Restricted";
    case 50: return "Never";
    default: return "\xE2\x80\x94";
    }
}

std::map<std::string, BgState, std::less<>> QueryBackground(const AdbClient& adb, std::string_view serial,
    const std::vector<std::string>& packages, std::stop_token st)
{
    if (packages.empty())
        return {};
    // for p in 'a' 'b'; do echo "@@$p"; cmd appops get "$p" RUN_ANY_IN_BACKGROUND; am get-standby-bucket "$p"; done
    ShellCommand cmd("for p in");
    for (const auto& p : packages)
        cmd.arg(p);
    cmd.raw("; do echo \"@@$p\"; cmd appops get \"$p\" RUN_ANY_IN_BACKGROUND 2>&1; am get-standby-bucket \"$p\" 2>&1; done");
    const auto r = adb.Shell(serial, cmd, { .timeout = 120s, .cancel = st });
    return ParseBgBatch(r.out);
}

std::vector<ProcessEntry> ParsePsApps(std::string_view text)
{
    std::vector<ProcessEntry> out;
    ForEachLine(text, [&](std::string_view line) {
        const auto tok = SplitWhitespace(line);
        if (tok.size() < 4)
            return;
        const auto pid = ParseInt<int>(tok[0]);
        const auto rss = ParseInt<uint64_t>(tok[1]);
        if (!pid || !rss)
            return; // header line
        // App users: u0_a123 (user 0), u10_a45 (work profile / secure folder).
        const auto user = tok[2];
        const auto us = user.find("_a");
        if (!user.starts_with("u") || us == std::string_view::npos)
            return;
        ProcessEntry e;
        e.pid = *pid;
        e.rssKb = *rss;
        e.user = std::string(user);
        e.name = std::string(tok[3]);
        out.push_back(std::move(e));
    });
    return out;
}

std::vector<RunningApp> GroupByPackage(const std::vector<ProcessEntry>& procs)
{
    std::map<std::string, RunningApp, std::less<>> by;
    for (const auto& p : procs) {
        const std::string pkg = p.name.substr(0, p.name.find(':'));
        auto& r = by[pkg];
        r.package = pkg;
        r.rssKb += p.rssKb;
        ++r.processes;
    }
    std::vector<RunningApp> out;
    for (auto& [k, v] : by)
        out.push_back(std::move(v));
    std::ranges::sort(out, [](const RunningApp& a, const RunningApp& b) { return a.rssKb > b.rssKb; });
    return out;
}

// ---- RestrictionLog ------------------------------------------------------------

RestrictionLog::RestrictionLog(std::filesystem::path file)
    : m_file(std::move(file))
{
    std::ifstream in(m_file, std::ios::binary);
    if (!in)
        return;
    try {
        const auto j = nlohmann::json::parse(in);
        for (const auto& e : j.value("restrictions", nlohmann::json::array())) {
            Restriction r;
            r.serial = e.value("serial", "");
            r.package = e.value("package", "");
            r.before.mode = e.value("prevMode", "allow");
            r.before.explicitMode = e.value("prevExplicit", false);
            r.before.bucket = e.value("prevBucket", -1);
            r.appliedBucket = e.value("appliedBucket", 0);
            r.appliedAt = e.value("appliedAt", "");
            if (!r.serial.empty() && !r.package.empty())
                m_entries.push_back(std::move(r));
        }
    } catch (const nlohmann::json::exception& e) {
        LogWarn("restrictions.json unreadable: {}", e.what());
    }
}

void RestrictionLog::Save() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : m_entries)
        arr.push_back({ { "serial", r.serial }, { "package", r.package }, { "prevMode", r.before.mode },
            { "prevExplicit", r.before.explicitMode }, { "prevBucket", r.before.bucket }, { "appliedBucket", r.appliedBucket },
            { "appliedAt", r.appliedAt } });
    const nlohmann::json j = { { "version", 1 }, { "restrictions", arr } };
    auto tmp = m_file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!out)
            return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, m_file, ec);
}

std::vector<Restriction> RestrictionLog::ForDevice(std::string_view serial) const
{
    std::lock_guard l(m_mutex);
    std::vector<Restriction> out;
    for (const auto& r : m_entries)
        if (r.serial == serial)
            out.push_back(r);
    return out;
}

std::vector<Restriction> RestrictionLog::All() const
{
    std::lock_guard l(m_mutex);
    return m_entries;
}

std::optional<Restriction> RestrictionLog::Find(std::string_view serial, std::string_view pkg) const
{
    std::lock_guard l(m_mutex);
    for (const auto& r : m_entries)
        if (r.serial == serial && r.package == pkg)
            return r;
    return std::nullopt;
}

void RestrictionLog::Add(Restriction r)
{
    std::lock_guard l(m_mutex);
    // Keep the ORIGINAL "before" state if the app was already restricted once.
    for (auto& e : m_entries)
        if (e.serial == r.serial && e.package == r.package) {
            e.appliedBucket = r.appliedBucket;
            e.appliedAt = r.appliedAt;
            Save();
            return;
        }
    m_entries.push_back(std::move(r));
    Save();
}

void RestrictionLog::Remove(std::string_view serial, std::string_view pkg)
{
    std::lock_guard l(m_mutex);
    std::erase_if(m_entries, [&](const Restriction& r) { return r.serial == serial && r.package == pkg; });
    Save();
}

size_t RestrictionLog::Count() const
{
    std::lock_guard l(m_mutex);
    return m_entries.size();
}

// ---- Restrict / Undo -------------------------------------------------------------

RestrictResult RestrictBackground(const AdbClient& adb, std::string_view serial, std::string_view pkg, int deviceSdk,
    std::stop_token st)
{
    RestrictResult res;
    const RunOptions opt{ .timeout = 20s, .cancel = st };

    // 1. Record the current state so Undo can restore it.
    const auto before = QueryBackground(adb, serial, { std::string(pkg) }, st);
    const auto it = before.find(pkg);
    if (it == before.end() || it->second.mode.empty()) {
        res.error = "Couldn't read the app's current background state.";
        return res;
    }

    // 2. Deny background running.
    const auto op = adb.Shell(serial, ShellCommand("cmd").arg("appops").arg("set").arg(pkg).arg("RUN_ANY_IN_BACKGROUND").arg("ignore"), opt);
    if (!op.ok() || op.out.find("Error") != std::string::npos) {
        res.error = op.err.empty() ? op.out : op.err;
        return res;
    }

    // 3. Lowest standby bucket this Android accepts.
    const bool hasRestricted = deviceSdk >= 31;
    const char* bucket = hasRestricted ? "restricted" : "rare";
    const auto sb = adb.Shell(serial, ShellCommand("am").arg("set-standby-bucket").arg(pkg).arg(bucket), opt);
    if (!sb.ok() || sb.out.find("Error") != std::string::npos)
        LogWarn("set-standby-bucket {} {} failed: {}{}", pkg, bucket, sb.out, sb.err); // appops part still applied

    res.ok = true;
    res.record.serial = std::string(serial);
    res.record.package = std::string(pkg);
    res.record.before = it->second;
    res.record.appliedBucket = hasRestricted ? kBucketRestricted : kBucketRare;
    res.record.appliedAt = NowIso();
    LogInfo("Restricted background for {} (was mode={}, bucket={})", pkg, it->second.mode, it->second.bucket);
    return res;
}

bool UndoRestriction(const AdbClient& adb, std::string_view serial, const Restriction& r, std::string* error, std::stop_token st)
{
    const RunOptions opt{ .timeout = 20s, .cancel = st };
    // An app that had no explicit mode goes back to "default", not a hard "allow".
    const std::string mode = r.before.explicitMode ? r.before.mode : "default";
    const auto op = adb.Shell(serial, ShellCommand("cmd").arg("appops").arg("set").arg(r.package).arg("RUN_ANY_IN_BACKGROUND").arg(mode), opt);
    if (!op.ok() || op.out.find("Error") != std::string::npos) {
        if (error)
            *error = op.err.empty() ? op.out : op.err;
        return false;
    }
    const int bucket = r.before.bucket > 0 ? r.before.bucket : kBucketActive;
    adb.Shell(serial, ShellCommand("am").arg("set-standby-bucket").arg(r.package).arg(std::to_string(bucket)), opt);
    LogInfo("Undid background restriction for {} (mode={}, bucket={})", r.package, mode, bucket);
    return true;
}

} // namespace ps
