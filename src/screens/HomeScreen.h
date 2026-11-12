#pragma once
#include "screens/IScreen.h"

#include <map>
#include <string>

namespace ps {

struct DeviceInfo;

// Full-window device picker: no adb / no phone / unauthorized / 1 phone / 2+ phones.
class HomeScreen final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    void DrawHeader(AppContext& ctx);
    void DrawFooter(AppContext& ctx);
    void DrawNoAdb(AppContext& ctx);
    void DrawWaiting(AppContext& ctx, bool unauthorized);
    void DrawReadyDevices(AppContext& ctx);
    void DrawUsbGuide();
    void Proceed(AppContext& ctx, const DeviceInfo& info);

    bool m_showGuide = false;
    float m_readyAnim = 0.0f; // 0 = big phone centred, 1 = small phone + card
    float m_lastContentHeight = 0.0f;
    std::map<std::string, bool, std::less<>> m_revealSerial;
};

} // namespace ps
