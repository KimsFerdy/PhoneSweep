#pragma once
#include "util/Log.h"

#include <cstdint>
#include <vector>

namespace ps {

struct AppContext;

// Dockable, filterable view over the session log (UI spec §4).
class LogPanel {
public:
    static constexpr const char* kWindowName = "Log###LogPanel";
    void Draw(AppContext& ctx);

private:
    enum Filter { All, Info, Warn, Error, Adb };
    bool Passes(const LogEntry& e) const;

    std::vector<LogEntry> m_entries;
    uint64_t m_next = 0;
    Filter m_filter = All;
    char m_search[128] = {};
    bool m_autoScroll = true;
};

} // namespace ps
