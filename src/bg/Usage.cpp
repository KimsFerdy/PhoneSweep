#include "bg/Usage.h"

#include "scan/AppModel.h"
#include "util/Strings.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace ps {

namespace {

std::optional<double> ParseDouble(std::string_view s)
{
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size())
        return std::nullopt;
    return v;
}

// "u0_a274" / "u95_a235" -> appId 10274 / 10235 (top/ps users).
std::optional<int> AppIdFromUser(std::string_view user)
{
    if (!user.starts_with("u"))
        return std::nullopt;
    const auto us = user.find("_a");
    if (us == std::string_view::npos)
        return std::nullopt;
    const auto n = ParseInt<int>(user.substr(us + 2));
    return n ? std::optional<int>(10000 + *n) : std::nullopt;
}

} // namespace

std::optional<uint64_t> ParseTopSize(std::string_view s)
{
    s = Trim(s);
    if (s.empty())
        return std::nullopt;
    double mult = 1024; // bare numbers are KB in toybox
    const char last = s.back();
    if (last == 'K' || last == 'k')
        mult = 1024.0;
    else if (last == 'M' || last == 'm')
        mult = 1024.0 * 1024;
    else if (last == 'G' || last == 'g')
        mult = 1024.0 * 1024 * 1024;
    else if (last == 'T' || last == 't')
        mult = 1024.0 * 1024 * 1024 * 1024;
    if (!std::isdigit(static_cast<unsigned char>(last)))
        s.remove_suffix(1);
    const auto v = ParseDouble(s);
    if (!v || *v < 0)
        return std::nullopt;
    return static_cast<uint64_t>(*v * mult);
}

TopSample ParseTop(std::string_view text)
{
    TopSample out;
    bool inTable = false;
    ForEachLine(text, [&](std::string_view line) {
        const auto t = Trim(line);
        if (t.empty())
            return;
        // Several frames (-n 2): keep only the last one, the first measures a tiny interval.
        if (t.starts_with("Tasks:") || t.starts_with("Threads:")) {
            inTable = false;
            out.processes.clear();
            return;
        }
        // "800%cpu  29%user ..." -> 8 cores
        if (const auto pc = t.find("%cpu"); pc != std::string_view::npos && !inTable) {
            if (const auto n = ParseInt<int>(t.substr(0, pc)))
                out.cores = *n / 100;
            return;
        }
        const auto tok = SplitWhitespace(t);
        if (tok.size() >= 2 && tok[0] == "PID") {
            inTable = true;
            return;
        }
        if (!inTable || tok.size() < 5)
            return;
        const auto pid = ParseInt<int>(tok[0]);
        const auto cpu = ParseDouble(tok[2]);
        const auto res = ParseTopSize(tok[3]);
        if (!pid || !cpu || !res)
            return;
        TopProcess p;
        p.pid = *pid;
        p.user = std::string(tok[1]);
        p.cpu = *cpu;
        p.resBytes = *res;
        // NAME is the rest of the line (kernel threads look like "[kworker/1:3]").
        p.name = std::string(t.substr(static_cast<size_t>(tok[4].data() - t.data())));
        out.processes.push_back(std::move(p));
    });
    return out;
}

std::vector<LiveApp> GroupTop(const TopSample& sample)
{
    std::map<std::string, LiveApp, std::less<>> by;
    for (const auto& p : sample.processes) {
        if (!AppIdFromUser(p.user))
            continue; // root/system/shell: not an app
        if (p.name.starts_with("["))
            continue;
        const std::string pkg = p.name.substr(0, p.name.find(':'));
        auto& a = by[pkg];
        a.package = pkg;
        a.cpu += p.cpu;
        a.ramBytes += p.resBytes;
        ++a.processes;
    }
    std::vector<LiveApp> out;
    for (auto& [k, v] : by)
        out.push_back(std::move(v));
    std::ranges::sort(out, [](const LiveApp& a, const LiveApp& b) {
        if (a.cpu != b.cpu)
            return a.cpu > b.cpu;
        return a.ramBytes > b.ramBytes;
    });
    return out;
}

BatteryDrain ParseBatteryStats(std::string_view text)
{
    BatteryDrain out;
    bool in = false;
    ForEachLine(text, [&](std::string_view line) {
        const auto t = Trim(line);
        if (!in) {
            if (t.starts_with("Estimated power use"))
                in = out.found = true;
            return;
        }
        if (t.empty()) {
            in = false; // section ends at the first blank line
            return;
        }
        if (t.starts_with("Capacity:")) {
            // "Capacity: 3300, Typical: 0, Computed drain: 113, actual drain: 66.0-99.0"
            const auto tok = SplitWhitespace(t);
            if (tok.size() >= 2)
                out.capacityMah = ParseInt<int>(std::string_view(tok[1]).substr(0, tok[1].find(','))).value_or(0);
            if (const auto cd = t.find("Computed drain:"); cd != std::string_view::npos) {
                auto rest = Trim(t.substr(cd + 15));
                rest = rest.substr(0, rest.find_first_of(", "));
                out.computedMah = ParseDouble(rest).value_or(0);
            }
            return;
        }
        if (t.starts_with("Global") || t.starts_with("Start time"))
            return;
        // "<key>: <mAh> ..." where key is "Uid u0a274" / "UID u0a274" / "Screen" / "Cell standby"
        const auto colon = t.find(':');
        if (colon == std::string_view::npos)
            return;
        std::string_view key = Trim(t.substr(0, colon));
        auto rest = Trim(t.substr(colon + 1));
        const auto valueTok = rest.substr(0, rest.find_first_of(" (\t"));
        const auto mAh = ParseDouble(valueTok);
        if (!mAh)
            return;
        DrainEntry e;
        e.mAh = *mAh;
        if (key.starts_with("Uid ") || key.starts_with("UID ")) {
            key.remove_prefix(4);
            e.key = std::string(key);
            // u0a274 -> user 0, appId 10274; "1000" -> plain uid
            if (key.starts_with("u")) {
                const auto a = key.find('a');
                const auto user = ParseInt<int>(key.substr(1, a == std::string_view::npos ? 0 : a - 1));
                const auto app = a == std::string_view::npos ? std::nullopt : ParseInt<int>(key.substr(a + 1));
                if (user && app) {
                    e.userId = *user;
                    e.appId = 10000 + *app;
                }
            } else if (const auto uid = ParseInt<int>(key)) {
                e.appId = *uid % 100000; // 1000, 1041, 0
                e.userId = *uid / 100000;
            }
        } else {
            e.key = std::string(key);
        }
        out.entries.push_back(std::move(e));
    });
    std::ranges::sort(out.entries, [](const DrainEntry& a, const DrainEntry& b) { return a.mAh > b.mAh; });
    return out;
}

std::string DrainLabel(const DrainEntry& e)
{
    if (e.key == "Screen")
        return "Screen";
    if (e.key == "Idle")
        return "Phone idle";
    if (e.key == "Cell standby")
        return "Mobile network standby";
    if (e.key == "Wifi")
        return "Wi-Fi";
    if (e.key == "Bluetooth")
        return "Bluetooth";
    if (e.key == "Phone")
        return "Phone calls";
    if (e.key == "Flashlight")
        return "Flashlight";
    if (e.key == "Camera")
        return "Camera (hardware)";
    if (e.appId == 0)
        return "Kernel (root)";
    if (e.appId == 1000)
        return "Android System";
    if (e.appId == 1001)
        return "Phone / radio";
    if (e.appId == 1013)
        return "Media server";
    if (e.appId >= 0 && e.appId < 10000)
        return "System service (uid " + std::to_string(e.appId) + ")";
    return e.key;
}

// ---- UsageStore ------------------------------------------------------------------

void UsageStore::SetLive(const std::vector<LiveApp>& apps)
{
    std::lock_guard l(m_mutex);
    for (auto& [k, u] : m_usage) {
        u.cpu.reset();
        u.ramBytes.reset();
    }
    for (const auto& a : apps) {
        auto& u = m_usage[a.package];
        u.cpu = a.cpu;
        u.ramBytes = a.ramBytes;
    }
}

void UsageStore::SetBattery(const BatteryDrain& drain, const std::vector<AppInfo>& apps)
{
    std::map<int, double> byAppId;
    for (const auto& e : drain.entries)
        if (e.appId >= 10000)
            byAppId[e.appId] += e.mAh;
    double total = drain.computedMah;
    if (total <= 0)
        for (const auto& e : drain.entries)
            total += e.mAh;
    std::lock_guard l(m_mutex);
    for (auto& [k, u] : m_usage) {
        u.batteryMah.reset();
        u.batteryShare.reset();
    }
    for (const auto& a : apps) {
        if (a.uid < 0)
            continue;
        auto it = byAppId.find(a.uid % 100000);
        if (it == byAppId.end())
            continue;
        auto& u = m_usage[a.package];
        u.batteryMah = it->second;
        if (total > 0)
            u.batteryShare = it->second / total;
    }
}

std::optional<AppUsage> UsageStore::Find(std::string_view package) const
{
    std::lock_guard l(m_mutex);
    auto it = m_usage.find(package);
    if (it == m_usage.end())
        return std::nullopt;
    return it->second;
}

void UsageStore::Clear()
{
    std::lock_guard l(m_mutex);
    m_usage.clear();
}

} // namespace ps
