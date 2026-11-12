#pragma once
#include "app/AppContext.h"
#include "cleanup/Cleanup.h"
#include "screens/CompressPanel.h"
#include "screens/IScreen.h"
#include "util/JobSystem.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps {

struct AppContext;

// Cleanup (M7): duplicate finder + junk sweep, with targeted, confirmed deletes.
class CleanupPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    struct DupShared {
        std::mutex mutex;
        bool running = false, done = false;
        DupProgress progress;
        std::vector<DupGroup> groups;
    };

    void DrawDuplicates(AppContext& ctx);
    void DrawJunk(AppContext& ctx);
    void StartFind(AppContext& ctx);
    void DrawConfirm(AppContext& ctx);
    const FileEntry* Lookup(const AppContext& ctx, std::string_view path);

    int m_tab = 0;
    CompressPanel m_compress;
    std::string m_serial;

    // Duplicates
    std::shared_ptr<DupShared> m_dup = std::make_shared<DupShared>();
    JobPtr m_dupJob;
    std::vector<DupGroup> m_groups;
    std::vector<bool> m_ticked;
    int m_minSize = 1; // index into kMinSizes
    bool m_searched = false;

    // Junk
    std::vector<JunkCategory> m_junk;
    std::vector<bool> m_junkTicked;
    std::vector<bool> m_junkOpen;
    uint64_t m_junkKey = ~0ull;

    // path -> FileStore index, rebuilt when the store changes
    std::unordered_map<std::string_view, uint32_t> m_byPath;
    uint64_t m_byPathVersion = ~0ull;

    // Pending confirmation
    bool m_confirmOpen = false;
    DeleteRequest m_pending;
    uint64_t m_pendingBytes = 0;
};

} // namespace ps
