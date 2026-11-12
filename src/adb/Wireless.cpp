#include "adb/Wireless.h"

#include "adb/AdbClient.h"
#include "util/Log.h"
#include "util/Strings.h"

#include <algorithm>
#include <cctype>
#include <thread>

namespace ps {

using namespace std::chrono_literals;

namespace {

bool IsHostChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
}

bool Contains(std::string_view hay, std::string_view needle)
{
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

std::optional<std::string> NormalizeAddress(std::string_view input, int defaultPort)
{
    input = Trim(input);
    if (input.empty())
        return std::nullopt;
    std::string_view host = input;
    int port = defaultPort;
    if (const auto colon = input.rfind(':'); colon != std::string_view::npos) {
        host = input.substr(0, colon);
        const auto p = ParseInt<int>(input.substr(colon + 1));
        if (!p || *p < 1 || *p > 65535)
            return std::nullopt;
        port = *p;
    }
    if (host.empty() || !std::ranges::all_of(host, IsHostChar))
        return std::nullopt;
    // All digits and dots? Then it must be a real IPv4 address.
    if (std::ranges::all_of(host, [](char c) { return c == '.' || std::isdigit(static_cast<unsigned char>(c)); })) {
        int parts = 0;
        std::string_view rest = host;
        while (true) {
            const auto dot = rest.find('.');
            const auto v = ParseInt<int>(rest.substr(0, dot));
            if (!v || *v < 0 || *v > 255)
                return std::nullopt;
            ++parts;
            if (dot == std::string_view::npos)
                break;
            rest.remove_prefix(dot + 1);
        }
        if (parts != 4)
            return std::nullopt;
    }
    return std::string(host) + ":" + std::to_string(port);
}

std::optional<std::string> ParseWifiIp(std::string_view text)
{
    // Blocks look like:
    // 9: wlan0: <BROADCAST,...> mtu 1500 ...
    //     inet 192.168.0.3/24 brd 192.168.0.255 scope global wlan0
    std::string iface;
    std::optional<std::string> wlan, other;
    ForEachLine(text, [&](std::string_view line) {
        const auto t = Trim(line);
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
            // "9: wlan0: <...>" -> wlan0 (strip "@if3" suffixes)
            const auto a = t.find(": ");
            const auto b = a == std::string_view::npos ? a : t.find(':', a + 2);
            iface = (a != std::string_view::npos && b != std::string_view::npos) ? std::string(t.substr(a + 2, b - a - 2)) : std::string();
            if (const auto at = iface.find('@'); at != std::string::npos)
                iface.resize(at);
            return;
        }
        if (!t.starts_with("inet "))
            return;
        auto addr = t.substr(5);
        addr = addr.substr(0, addr.find_first_of("/ "));
        if (addr.starts_with("127.") || iface == "lo")
            return;
        if (iface.starts_with("wlan") || iface.starts_with("swlan")) {
            if (!wlan)
                wlan = std::string(addr);
        } else if (!iface.starts_with("rmnet") && !iface.starts_with("ccmni") && !iface.starts_with("dummy") && !iface.starts_with("p2p")) {
            if (!other)
                other = std::string(addr);
        }
    });
    return wlan ? wlan : other;
}

WirelessResult ParseConnectOutput(std::string_view out, std::string_view address)
{
    WirelessResult r;
    r.address = std::string(address);
    const auto t = Trim(out);
    if (Contains(t, "connected to") && !Contains(t, "cannot") && !Contains(t, "failed")) {
        r.ok = true;
        r.message = Contains(t, "already") ? "Already connected" : "Connected";
        return r;
    }
    if (Contains(t, "Connection refused"))
        r.message = "The phone refused the connection. Is wireless debugging on (or was \"Switch to Wi-Fi\" done over USB first)?";
    else if (Contains(t, "timed out") || Contains(t, "No route") || Contains(t, "unreachable"))
        r.message = "The phone didn't answer. Check that the PC and the phone are on the same Wi-Fi network.";
    else if (Contains(t, "authenticate") || Contains(t, "unauthorized"))
        r.message = "Connected, but the phone hasn't allowed this PC yet. Check the phone's screen.";
    else
        r.message = t.empty() ? std::string("adb connect gave no answer") : std::string(t);
    return r;
}

WirelessResult ParsePairOutput(std::string_view out)
{
    WirelessResult r;
    const auto t = Trim(out);
    if (Contains(t, "Successfully paired")) {
        r.ok = true;
        r.message = "Paired";
        return r;
    }
    if (Contains(t, "Wrong password") || Contains(t, "wrong password") || Contains(t, "protocol fault"))
        r.message = "The pairing code was wrong or has expired. Open the pairing dialog on the phone again.";
    else
        r.message = t.empty() ? std::string("adb pair gave no answer") : std::string(t);
    return r;
}

WirelessResult ConnectWireless(const AdbClient& adb, const std::string& address, std::stop_token st)
{
    const auto res = adb.Run({ "connect", address }, { .timeout = 15s, .cancel = st });
    auto r = ParseConnectOutput(res.out + res.err, address);
    LogInfo("adb connect {}: {}", address, r.message);
    return r;
}

WirelessResult PairWireless(const AdbClient& adb, const std::string& address, const std::string& code, std::stop_token st)
{
    const auto res = adb.Run({ "pair", address, code }, { .timeout = 30s, .cancel = st });
    auto r = ParsePairOutput(res.out + res.err);
    r.address = address;
    LogInfo("adb pair {}: {}", address, r.message);
    return r;
}

void DisconnectWireless(const AdbClient& adb, const std::string& address)
{
    adb.Run({ "disconnect", address }, { .timeout = 10s });
    LogInfo("adb disconnect {}", address);
}

WirelessResult SwitchToWifi(const AdbClient& adb, const std::string& usbSerial, std::stop_token st)
{
    WirelessResult r;
    const auto ipOut = adb.Shell(usbSerial, ShellCommand("ip").arg("-f").arg("inet").arg("addr").arg("show"), { .timeout = 10s, .cancel = st });
    const auto ip = ParseWifiIp(ipOut.out);
    if (!ip) {
        r.message = "The phone has no Wi-Fi address. Connect it to the same Wi-Fi network as this PC.";
        return r;
    }
    r.address = *ip + ":5555";
    // Restarts adbd on the phone listening on TCP 5555 (the USB link blips for a moment).
    const auto t = adb.Run({ "-s", usbSerial, "tcpip", "5555" }, { .timeout = 15s, .cancel = st });
    LogInfo("adb tcpip 5555 on {}: {}", usbSerial, Trim(t.out + t.err));
    if (!Contains(t.out + t.err, "restarting") && !t.ok()) {
        r.message = "The phone didn't switch to Wi-Fi mode: " + std::string(Trim(t.out + t.err));
        return r;
    }
    // adbd needs a moment to come back up.
    for (int attempt = 0; attempt < 8 && !st.stop_requested(); ++attempt) {
        std::this_thread::sleep_for(attempt == 0 ? 1500ms : 1000ms);
        auto c = ConnectWireless(adb, r.address, st);
        if (c.ok)
            return c;
        r.message = c.message;
    }
    return r;
}

} // namespace ps
