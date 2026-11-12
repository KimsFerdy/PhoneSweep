#pragma once
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace ps {

class AdbClient;

// ---- Pure helpers (unit-tested) ----------------------------------------------

// "192.168.0.3" -> "192.168.0.3:5555", "192.168.0.3:37199" unchanged, junk -> nullopt.
// Accepts IPv4 or a host name; the port must be 1..65535.
std::optional<std::string> NormalizeAddress(std::string_view input, int defaultPort = 5555);

// Picks the phone's Wi-Fi IPv4 from `ip -f inet addr show`: wlan* first, then any
// other non-loopback, non-mobile-data interface. nullopt when none.
std::optional<std::string> ParseWifiIp(std::string_view ipAddrOutput);

// Interprets `adb connect` / `adb pair` output. ok + "" or !ok + a readable reason.
struct WirelessResult {
    bool ok = false;
    std::string address; // ip:port that is (now) connected
    std::string message; // human-readable status or error
};
WirelessResult ParseConnectOutput(std::string_view out, std::string_view address);
WirelessResult ParsePairOutput(std::string_view out);

// ---- Live -----------------------------------------------------------------------

// Android 10 and older (works on newer too): the phone is on USB; switch its adbd to
// TCP 5555 and connect to its Wi-Fi address. The cable can be unplugged afterwards.
WirelessResult SwitchToWifi(const AdbClient& adb, const std::string& usbSerial, std::stop_token st);

WirelessResult ConnectWireless(const AdbClient& adb, const std::string& address, std::stop_token st);
// Android 11+: Developer options -> Wireless debugging -> "Pair device with pairing code".
WirelessResult PairWireless(const AdbClient& adb, const std::string& address, const std::string& code, std::stop_token st);
void DisconnectWireless(const AdbClient& adb, const std::string& address);

} // namespace ps
