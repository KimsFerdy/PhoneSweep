#pragma once
#include "adb/Wireless.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace ps {

struct AppContext;

// M9: connect phones over Wi-Fi. Android 10 and older switch over from USB
// ("adb tcpip"); Android 11+ can pair with a code, no cable at all.
class WirelessDialog {
public:
    void Draw(AppContext& ctx);

    // Remembers a connected address (and the phone's name) for one-click reconnects.
    static void Remember(AppContext& ctx, const std::string& address, const std::string& name);

private:
    struct Shared {
        std::mutex mutex;
        bool busy = false;
        std::string action; // "Switching SM-G960F to Wi-Fi…"
        std::optional<WirelessResult> result;
        bool pairStep = false; // result came from a pair (next: connect)
    };
    void Run(AppContext& ctx, std::string action, std::function<WirelessResult(std::stop_token)> fn, bool pair = false);

    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::optional<WirelessResult> m_last;
    char m_address[64] = {};
    char m_pairAddress[64] = {};
    char m_pairCode[16] = {};
    bool m_wasOpen = false;
};

} // namespace ps
