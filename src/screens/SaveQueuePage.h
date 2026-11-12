#pragma once
#include "screens/IScreen.h"

namespace ps {

// Save queue (UI spec §9): totals, speed, ETA, controls, per-file states.
class SaveQueuePage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;
};

} // namespace ps
