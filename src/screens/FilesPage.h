#pragma once
#include "screens/IScreen.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ps {

struct AppContext;
class FileStore;

// Files screen (UI spec §6): categories/sources sidebar, filter toolbar,
// sortable list or grid, sticky selection bar, preview drawer.
class FilesPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    enum Column { ColSelect, ColName, ColFolder, ColSource, ColSize, ColDate };

    void DrawSidebar(AppContext& ctx, float height);
    void DrawToolbar(AppContext& ctx);
    void DrawList(AppContext& ctx, float height);
    void DrawGrid(AppContext& ctx, float height);
    void DrawSelectionBar(AppContext& ctx);
    void DrawDrawer(AppContext& ctx, float height);
    void DrawStatusArea(AppContext& ctx, float height);
    void HandleKeys(AppContext& ctx);
    void HandleFocusRequest(AppContext& ctx);

    void RebuildView(const FileStore& store);
    void Sort(const FileStore& store);
    void Invalidate() { m_viewDirty = true; }
    // Click on a row/tile: toggle, or extend from the anchor with Shift.
    void ClickRow(FileStore& store, int row, bool shift);

    // Filters
    int m_category = -1; // -1 = All
    int m_source = -1;   // -1 = any
    bool m_showProtected = false;
    char m_search[256] = {};
    int m_sizeFilter = 0;
    int m_dateFilter = 0;
    bool m_grid = false;
    bool m_focusSearch = false;

    // Sorting
    int m_sortColumn = ColSize;
    bool m_sortAscending = false;

    // Cached view (indices into FileStore::Files()).
    std::vector<uint32_t> m_view;
    uint64_t m_viewVersion = ~0ull;
    bool m_viewDirty = true;
    double m_lastRebuild = 0.0;
    uint64_t m_viewBytes = 0;

    int m_anchorRow = -1;
    int m_focusRow = -1;
    int64_t m_preview = -1;  // file index shown in the drawer
    int m_scrollToRow = -1;
};

} // namespace ps
