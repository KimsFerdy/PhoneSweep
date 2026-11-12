#pragma once
#include "adb/Device.h"
#include "scan/AppModel.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps {

class FileStore;
class VerifiedIndex;

struct ReportInputs {
    DeviceInfo device;
    bool maskSerial = true;
    bool fullPermissions = true;
    const FileStore* files = nullptr;
    const std::vector<AppInfo>* apps = nullptr;
    const VerifiedIndex* verified = nullptr;
    size_t permissionDenied = 0;
    size_t unreadableNames = 0;
    std::filesystem::path sessionLog; // copied to logs/ (serial masked if asked)
    int64_t now = 0;                  // unix seconds (0 = current time)
};

// scan_report.md: device, storage, categories, sources, top 50 files, top folders,
// app risk summary. Valid Markdown; table cells escaped.
std::string FormatScanReport(const ReportInputs& in);

// applist.txt for every user app (system apps are noise for "is this app shady?").
std::string FormatUserAppList(const ReportInputs& in);

enum ExportParts : unsigned { Export_AppList = 1, Export_ScanReport = 2, Export_Logs = 4, Export_All = 7 };

struct ExportResult {
    bool ok = false;
    std::filesystem::path dir;
    std::vector<std::string> files; // relative names written
    std::string error;
};

// Writes <reportsRoot>/<YYYY-MM-DD_HHMMSS>/{applist.txt, scan_report.md, logs/, export.json}.
ExportResult ExportReports(const ReportInputs& in, const std::filesystem::path& reportsRoot, unsigned parts);

struct ExportInfo {
    std::filesystem::path dir;
    std::string created; // from export.json, else folder name
    std::string device;
    uint64_t bytes = 0;
    std::vector<std::string> files;
};
// Previous exports, newest first.
std::vector<ExportInfo> ListExports(const std::filesystem::path& reportsRoot);

// Markdown table cell: escapes | and turns invalid UTF-8 / control chars into safe text.
std::string MdCell(std::string_view s);

} // namespace ps
