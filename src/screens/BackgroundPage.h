#pragma once
#include "bg/Background.h"
#include "bg/Usage.h"
#include "screens/IScreen.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ps {

struct AppContext;

// Background & Startup (UI spec §8): boot apps with reversible Restrict,
// Running now (live CPU/RAM, 2 s refresh, sparklines) with Force stop, Battery (estimated drain).
class BackgroundPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    struct Shared {
        std::mutex mutex;
        // Boot tab
        std::map<std::string, BgState, std::less<>> states;
        bool statesLoading = false;
        bool statesLoaded = false;
        // Running tab
        std::vector<RunningApp> running;
        bool runningLoading = false;
        bool runningLoaded = false;
        std::string runningError;
        // Live (M10)
        std::vector<LiveApp> live;
        int cores = 0;
        uint64_t liveSeq = 0; // bumps with every sample
        // Battery (M10)
        BatteryDrain drain;
        bool batteryLoading = false, batteryLoaded = false;
        std::string batteryError;
        // Action results: (package, message, ok)
        std::vector<std::tuple<std::string, std::string, bool>> results;
        std::set<std::string, std::less<>> busy; // packages with an action in flight
    };

    void DrawBootTab(AppContext& ctx, float height);
    void DrawRunningTab(AppContext& ctx, float height);
    void DrawBatteryTab(AppContext& ctx, float height);
    void LoadBattery(AppContext& ctx);
    void LoadStates(AppContext& ctx, std::vector<std::string> packages);
    void LoadRunning(AppContext& ctx);
    void Restrict(AppContext& ctx, const std::vector<std::string>& packages);
    void Undo(AppContext& ctx, const std::vector<std::string>& packages);
    bool Busy(std::string_view pkg);

    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::string m_serial;
    uint64_t m_appsVersion = ~0ull;
    int m_tab = 0;
    bool m_includeSystem = false;
    std::set<std::string, std::less<>> m_selected;

    // Live sampling (UI thread).
    bool m_livePaused = false;
    double m_lastPoll = -100.0;
    uint64_t m_seenSeq = 0;
    int m_liveSort = 0; // 0 CPU, 1 RAM
    struct History {
        std::deque<float> cpu, ram; // last 30 samples (1 min)
    };
    std::map<std::string, History, std::less<>> m_history;
    double m_batteryPublished = -1.0;
};

} // namespace ps
