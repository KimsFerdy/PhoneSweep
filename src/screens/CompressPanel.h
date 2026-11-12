#pragma once
#include "compress/Compress.h"
#include "util/JobSystem.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ps {

struct AppContext;

// Cleanup -> Compress (M8): shrink big camera videos / photos, with a choice of
// where the compressed copy ends up (phone + PC, phone only, PC only).
class CompressPanel {
public:
    void Draw(AppContext& ctx);

private:
    struct MediaShared {
        std::mutex mutex;
        bool running = false, done = false;
        std::string error;
        std::map<std::string, MediaInfo, std::less<>> media;
        std::optional<bool> qsv; // Intel Quick Sync available?
    };

    void StartQuery(AppContext& ctx);
    void StartCompress(AppContext& ctx);
    void DrawList(AppContext& ctx);
    void DrawRunning(AppContext& ctx);
    void DrawResults(AppContext& ctx);
    void DrawChoice(AppContext& ctx);

    std::string m_serial;
    std::shared_ptr<MediaShared> m_shared = std::make_shared<MediaShared>();
    JobPtr m_queryJob;
    bool m_queried = false;
    std::map<std::string, MediaInfo, std::less<>> m_media;
    std::optional<bool> m_qsv;

    std::vector<CompressCandidate> m_cands;
    uint64_t m_candKey = ~0ull;
    std::set<std::string, std::less<>> m_ticked; // by path, survives recomputes
    int m_filter = 0;                           // 0 all, 1 videos, 2 photos

    bool m_choiceOpen = false;
    int m_outcome = 0; // CompressOutcome
    bool m_useGpu = true;

    std::shared_ptr<CompressProgress> m_progress;
    JobPtr m_runJob;
};

} // namespace ps
