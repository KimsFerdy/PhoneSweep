#pragma once
#include "nuke/NukePlan.h"
#include "nuke/Nuker.h"
#include "screens/IScreen.h"
#include "util/JobSystem.h"

#include <filesystem>
#include <memory>
#include <string>

namespace ps {

struct AppContext;

// Nuke (UI spec §10 + Nuke Protector). The only screen with danger styling.
class NukePage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    void RebuildPlan(AppContext& ctx);
    void DrawPlan(AppContext& ctx);
    void DrawFileReview(AppContext& ctx, const NukeGroup& group, bool protectedList);
    void DrawAppReview(AppContext& ctx);
    void DrawConfirm(AppContext& ctx);
    void DrawRunning(AppContext& ctx);
    void DrawResult(AppContext& ctx);
    void DryRun(AppContext& ctx);
    void Start(AppContext& ctx);
    void Finish(AppContext& ctx);

    NukePlan m_plan;
    NukeOverrides m_ov;
    uint64_t m_ovVersion = 0;
    uint64_t m_planKey = ~0ull; // cache key for the plan

    bool m_showFiles = false, m_showApps = false, m_showProtected = false;
    char m_search[128] = {};
    char m_confirm[16] = {};
    uint64_t m_dryRunFingerprint = 0;
    std::filesystem::path m_lastPlanFile;

    std::shared_ptr<NukeProgress> m_progress;
    JobPtr m_job;
    bool m_finishedHandled = false;
    std::string m_serial;
};

} // namespace ps
