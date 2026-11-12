#pragma once
#include "scan/FileModel.h"
#include "scan/ScanParse.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps {

class AdbClient;

struct ScanOptions {
    bool includeSdCard = true;
    uint64_t minSizeBytes = 0;
};

enum class ListMethod { None, FindPrintf, FindExecStat };
const char* ToString(ListMethod m);

// Callbacks run on the scanning thread.
struct ScanSink {
    std::function<void(const std::vector<Volume>&)> onVolumes;
    std::function<void(std::vector<FileEntry>&&)> onBatch;
    std::function<void(std::string phase)> onPhase;
    std::function<void(std::vector<MediaRow>&&)> onMediaStore; // MIME enrichment, once at the end
};

struct ScanOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string error;           // plain-English, shown in the UI
    std::string errorDetail;     // adb command + stderr
    ListMethod method = ListMethod::None;
    size_t files = 0;
    size_t permissionDenied = 0; // paths find couldn't enter
    size_t unreadableNames = 0;  // names find listed but stat couldn't open (bad encoding)
};

// Lists every file on the phone's shared storage (+ SD cards), probing what the
// device's toybox supports and falling back gracefully.
class FileScanner {
public:
    static std::vector<Volume> DiscoverVolumes(const AdbClient& adb, std::string_view serial, bool includeSd, std::stop_token st);
    static ListMethod Probe(const AdbClient& adb, std::string_view serial, const std::string& root, std::stop_token st);
    static ScanOutcome Run(const AdbClient& adb, std::string_view serial, const ScanOptions& opts, const ScanSink& sink,
        std::stop_token st);
};

} // namespace ps
