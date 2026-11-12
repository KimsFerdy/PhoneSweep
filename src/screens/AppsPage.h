#pragma once
#include "apps/AppActions.h"
#include "screens/IScreen.h"
#include "util/JobSystem.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ps {

struct AppContext;
class AppSession;
struct AppInfo;

// Apps screen (UI spec §7): user/system tabs, risk badges, detail drawer.
class AppsPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

    // Overview "N apps flagged" -> open with the Flagged filter.
    static void RequestFlaggedFilter() { s_wantFlagged = true; }

private:
    enum Filter { All, Flagged, Sideloaded, NoIcon, Uninstall };
    enum SortBy { ByRisk, BySize, ByName, ByInstalled };

    void DrawToolbar(AppContext& ctx);
    void DrawTable(AppContext& ctx, float height);
    void DrawSelectionBar(AppContext& ctx);
    void DrawDrawer(AppContext& ctx, float height);
    void DrawContextMenu(AppContext& ctx, const AppInfo& a);
    void OpenUninstall(AppContext& ctx, const AppInfo& a, UninstallMode mode);
    void DrawUninstall(AppContext& ctx);
    static void AskAi(AppContext& ctx, const AppInfo& a);
    void RebuildView(const AppSession& apps);
    const AppInfo* FindApp(const AppSession& apps, std::string_view pkg) const;
    std::vector<const AppInfo*> SelectedApps(const AppSession& apps) const;

    int m_tab = 0; // 0 user, 1 system
    Filter m_filter = All;
    SortBy m_sort = ByRisk;
    char m_search[128] = {};

    std::vector<int> m_view;
    uint64_t m_viewVersion = ~0ull;
    bool m_dirty = true;

    std::set<std::string, std::less<>> m_selected;
    std::string m_drawer; // package shown in the detail drawer
    bool m_confirmLabels = false;

    // Right-click uninstall (quick / full).
    std::string m_uninstallPkg;
    UninstallMode m_uninstallMode = UninstallMode::Quick;
    bool m_uninstallOpen = false;
    bool m_backupApk = true;
    std::vector<AppFolder> m_appFolders;
    std::shared_ptr<UninstallProgress> m_uninstall;
    JobPtr m_uninstallJob;

    static inline bool s_wantFlagged = false;
};

} // namespace ps
