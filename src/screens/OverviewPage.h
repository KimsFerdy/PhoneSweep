#pragma once
#include "screens/IScreen.h"

#include <cstdint>
#include <vector>

namespace ps {

// Dashboard (UI spec §5): storage donut, quick actions, biggest files, attention list.
class OverviewPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    void DrawStorage(AppContext& ctx, float w, float h);
    void DrawQuickActions(AppContext& ctx, float w, float h);
    void DrawBiggest(AppContext& ctx, float w, float h);
    void DrawAttention(AppContext& ctx, float w, float h);

    std::vector<uint32_t> m_biggest;
    uint64_t m_biggestVersion = ~0ull;
    bool m_confirmTrim = false;
    float m_storageNeed = 0.0f;
};

} // namespace ps
