#pragma once
#include "app/AppContext.h"
#include "screens/IScreen.h"
#include "screens/LogPanel.h"

#include <array>
#include <memory>

namespace ps {

// Device workspace: top bar, sidebar nav, docked content + log, status bar.
class WorkspaceScreen final : public IScreen {
public:
    WorkspaceScreen();
    ~WorkspaceScreen() override;
    void Draw(AppContext& ctx) override;

private:
    void DrawTopBar(AppContext& ctx);
    void DrawDisconnectBanner(AppContext& ctx);
    void DrawScanStrip(AppContext& ctx);
    void DrawSidebar(AppContext& ctx, float height);
    void DrawDockArea(AppContext& ctx, float width, float height);
    void DrawStatusBar(AppContext& ctx);
    void DrawPage(AppContext& ctx);
    void BuildDockLayout(unsigned dockId, float width, float height);

    LogPanel m_log;
    bool m_layoutBuilt = false;
    bool m_logWasShown = false;
    std::array<std::unique_ptr<IScreen>, static_cast<size_t>(Page::Count)> m_pages;
};

} // namespace ps
