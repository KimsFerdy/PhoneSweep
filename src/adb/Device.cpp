#include "adb/Device.h"

#include "adb/AdbClient.h"
#include "util/Strings.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace ps {

std::vector<DeviceEntry> ParseDevicesList(std::string_view text)
{
    std::vector<DeviceEntry> out;
    ForEachLine(text, [&](std::string_view line) {
        line = Trim(line);
        if (line.empty() || line.starts_with("List of devices") || line.starts_with("*"))
            return;
        const auto tokens = SplitWhitespace(line);
        if (tokens.size() < 2)
            return;

        DeviceEntry e;
        e.serial = tokens[0];
        e.rawState = tokens[1];
        if (tokens[1] == "device")
            e.state = DeviceEntry::State::Device;
        else if (tokens[1] == "unauthorized")
            e.state = DeviceEntry::State::Unauthorized;
        else if (tokens[1] == "offline")
            e.state = DeviceEntry::State::Offline;

        for (size_t i = 2; i < tokens.size(); ++i) {
            const auto kv = tokens[i];
            const auto colon = kv.find(':');
            if (colon == std::string_view::npos)
                continue;
            const auto key = kv.substr(0, colon);
            const auto val = std::string(kv.substr(colon + 1));
            if (key == "model") {
                e.model = val;
                std::ranges::replace(e.model, '_', '-');
            } else if (key == "product") {
                e.product = val;
            } else if (key == "transport_id") {
                e.transportId = val;
            }
        }
        out.push_back(std::move(e));
    });
    return out;
}

std::map<std::string, std::string, std::less<>> ParseGetprop(std::string_view text)
{
    std::map<std::string, std::string, std::less<>> props;
    ForEachLine(text, [&](std::string_view line) {
        // [key]: [value]
        line = Trim(line);
        if (line.size() < 6 || line.front() != '[')
            return;
        const auto keyEnd = line.find("]: [");
        if (keyEnd == std::string_view::npos || line.back() != ']')
            return;
        props.emplace(std::string(line.substr(1, keyEnd - 1)),
            std::string(line.substr(keyEnd + 4, line.size() - keyEnd - 5)));
    });
    return props;
}

std::string DecodeOneUi(std::string_view oneuiProp, std::string_view sepProp)
{
    if (auto v = ParseInt<int>(Trim(oneuiProp)); v && *v > 0) {
        // 60100 -> 6.1, 50101 -> 5.1.1
        const int major = *v / 10000, minor = (*v / 100) % 100, patch = *v % 100;
        return patch ? std::format("{}.{}.{}", major, minor, patch) : std::format("{}.{}", major, minor);
    }
    if (auto v = ParseInt<int>(Trim(sepProp)); v && *v >= 100000) {
        // SEP 10.0 = One UI 1.0, SEP 10.5 = 1.5, SEP 11.0 = 2.0 ...
        // Encoded as major*10000 + minor*100: 100500 = SEP 10.5.
        const int sepMajor = *v / 10000, sepMinor = (*v % 10000) / 100;
        return std::format("{}.{}", sepMajor - 9, sepMinor);
    }
    return {};
}

BatteryInfo ParseDumpsysBattery(std::string_view text)
{
    BatteryInfo b;
    bool inMainBlock = true;
    ForEachLine(text, [&](std::string_view raw) {
        // Only the first indented block belongs to the standard battery service;
        // Samsung appends extra blocks with look-alike keys (e.g. "charge counter").
        if (!raw.empty() && raw.front() != ' ' && !raw.starts_with("Current Battery Service state"))
            inMainBlock = false;
        if (!inMainBlock)
            return;
        const auto line = Trim(raw);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            return;
        const auto key = line.substr(0, colon);
        const auto val = Trim(line.substr(colon + 1));
        if (key == "level")
            b.level = ParseInt<int>(val);
        else if (key == "status")
            b.charging = (val == "2"); // BATTERY_STATUS_CHARGING
        else if (key == "temperature")
            if (auto t = ParseInt<int>(val))
                b.tempC = *t / 10.0;
    });
    return b;
}

std::optional<uint64_t> ParseMemTotalKb(std::string_view meminfo)
{
    std::optional<uint64_t> result;
    ForEachLine(meminfo, [&](std::string_view line) {
        if (result || !line.starts_with("MemTotal:"))
            return;
        const auto tokens = SplitWhitespace(line);
        if (tokens.size() >= 2)
            result = ParseInt<uint64_t>(tokens[1]);
    });
    return result;
}

int MarketingRamGB(uint64_t memTotalKb)
{
    // The kernel reserves memory, so MemTotal is always under the box number.
    static constexpr std::array kSizes = { 1, 2, 3, 4, 6, 8, 10, 12, 16, 18, 24, 32 };
    const double gib = static_cast<double>(memTotalKb) / (1024.0 * 1024.0);
    for (int s : kSizes)
        if (gib <= s)
            return s;
    return static_cast<int>(gib + 0.999);
}

std::optional<StorageStats> ParseDfK(std::string_view text)
{
    std::optional<StorageStats> result;
    ForEachLine(text, [&](std::string_view line) {
        const auto tokens = SplitWhitespace(line);
        if (tokens.size() < 4)
            return;
        auto total = ParseInt<uint64_t>(tokens[1]);
        auto used = ParseInt<uint64_t>(tokens[2]);
        auto avail = ParseInt<uint64_t>(tokens[3]);
        if (total && used && avail)
            result = StorageStats{ *total * 1024, *used * 1024, *avail * 1024 };
    });
    return result;
}

std::string MaskSerial(std::string_view serial)
{
    if (serial.size() <= 4)
        return std::string(serial);
    std::string out;
    for (size_t i = 0; i < std::min<size_t>(serial.size() - 4, 8); ++i)
        out += "\xE2\x80\xA2"; // •
    out += serial.substr(serial.size() - 4);
    return out;
}

std::string CapitalizeBrand(std::string_view brand)
{
    std::string s(brand);
    if (!s.empty())
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::optional<DeviceInfo> QueryDeviceInfo(const AdbClient& adb, std::string_view serial)
{
    ShellCommand cmd("getprop");
    cmd.raw("; echo @@PS@@; dumpsys battery; echo @@PS@@; cat /proc/meminfo; echo @@PS@@; df -k /data");
    const AdbResult r = adb.Shell(serial, cmd, { .timeout = 15s });
    if (r.status != AdbResult::Status::Ok || r.out.empty())
        return std::nullopt;

    std::array<std::string_view, 4> parts{};
    {
        std::string_view rest = r.out;
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto sep = rest.find("@@PS@@");
            parts[i] = rest.substr(0, sep);
            if (sep == std::string_view::npos)
                break;
            rest.remove_prefix(sep + 6);
        }
    }

    const auto props = ParseGetprop(parts[0]);
    auto prop = [&](std::string_view key) -> std::string {
        auto it = props.find(key);
        return it != props.end() ? it->second : std::string{};
    };

    DeviceInfo info;
    info.serial = std::string(serial);
    info.model = prop("ro.product.model");
    info.brand = CapitalizeBrand(prop("ro.product.brand"));
    info.displayName = prop("ro.product.marketname");
    if (info.displayName.empty())
        info.displayName = info.brand.empty() ? info.model : info.brand + " " + info.model;
    info.android = prop("ro.build.version.release");
    info.sdk = ParseInt<int>(prop("ro.build.version.sdk")).value_or(0);
    info.oneUi = DecodeOneUi(prop("ro.build.version.oneui"), prop("ro.build.version.sep"));
    info.securityPatch = prop("ro.build.version.security_patch");
    info.hwSerial = prop("ro.serialno");
    if (info.hwSerial.empty())
        info.hwSerial = prop("ro.boot.serialno");

    const auto bat = ParseDumpsysBattery(parts[1]);
    info.batteryLevel = bat.level;
    info.charging = bat.charging;
    info.batteryTempC = bat.tempC;

    if (auto kb = ParseMemTotalKb(parts[2]))
        info.ramMarketingGB = MarketingRamGB(*kb);
    info.storage = ParseDfK(parts[3]);
    info.wireless = info.serial.find(':') != std::string::npos || info.serial.starts_with("adb-");
    return info;
}

} // namespace ps
