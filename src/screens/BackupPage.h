#pragma once
#include "backup/ZipBackup.h"
#include "restore/Restore.h"
#include "screens/IScreen.h"
#include "screens/SaveQueuePage.h"
#include "util/JobSystem.h"

#include <memory>
#include <mutex>
#include <vector>

namespace ps {

// Backup (M8/M9): the Save queue, Restore (push a backup back to a phone) and Archives (zip backups).
class BackupPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    struct PlanShared {
        std::mutex mutex;
        bool running = false, done = false;
        std::vector<RestoreItem> items;
    };

    void DrawRestore(AppContext& ctx);
    void DrawRestoreRunning(AppContext& ctx);
    void StartPlan(AppContext& ctx);
    void StartRestore(AppContext& ctx);
    void DrawArchives(AppContext& ctx);
    void StartZip(AppContext& ctx, const std::filesystem::path& folder);

    int m_tab = 0;
    SaveQueuePage m_queue;

    std::vector<BackupFolderInfo> m_folders;
    bool m_foldersLoaded = false;
    int m_folder = -1;
    std::string m_serial;

    std::shared_ptr<PlanShared> m_plan = std::make_shared<PlanShared>();
    JobPtr m_planJob;
    std::vector<RestoreItem> m_items;
    bool m_planned = false;
    int m_policy = 0; // RestorePolicy
    bool m_confirmOpen = false;
    bool m_autoPlan = false; // dev preset

    std::shared_ptr<RestoreProgress> m_progress;
    JobPtr m_runJob;

    std::shared_ptr<ZipProgress> m_zip;
    JobPtr m_zipJob;
};

} // namespace ps
