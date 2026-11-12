#pragma once
#include "report/Exporter.h"
#include "screens/IScreen.h"

#include <filesystem>
#include <vector>

namespace ps {

// Reports (UI spec §11): AI-neutral exports, previous exports.
class ReportsPage final : public IScreen {
public:
    void Draw(AppContext& ctx) override;

private:
    std::vector<ExportInfo> m_exports;
    double m_lastList = -100.0;
    std::filesystem::path m_listedRoot, m_pendingDelete;
};

} // namespace ps
