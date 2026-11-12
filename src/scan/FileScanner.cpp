#include "scan/FileScanner.h"

#include "adb/AdbClient.h"
#include "adb/Device.h"
#include "scan/Categorize.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace ps {

namespace {

constexpr const char* kInternalRoot = "/storage/emulated/0/";
// Size, allocated space, mtime, path. Allocated space matters for sparse files
// (e.g. Samsung's .thumbdata files report 5.8 GB but occupy ~2 MB).
constexpr const char* kPrintfFormat = "%s\\t%k\\t%T@\\t%p\\n"; // find -printf escapes
constexpr const char* kStatFormat = "%s\t%b\t%B\t%Y\t%n";       // literal tabs: toybox stat doesn't expand \t
constexpr size_t kBatch = 1024;
constexpr auto kListTimeout = std::chrono::minutes(15);

size_t CountLinesContaining(std::string_view text, std::string_view needle)
{
    size_t n = 0;
    ForEachLine(text, [&](std::string_view line) { n += line.find(needle) != std::string_view::npos; });
    return n;
}

} // namespace

const char* ToString(ListMethod m)
{
    switch (m) {
    case ListMethod::FindPrintf: return "find -printf";
    case ListMethod::FindExecStat: return "find -exec stat";
    default: return "none";
    }
}

std::vector<Volume> FileScanner::DiscoverVolumes(const AdbClient& adb, std::string_view serial, bool includeSd, std::stop_token st)
{
    std::vector<Volume> vols;
    vols.push_back({ kInternalRoot, "Internal", false });
    if (includeSd) {
        const auto r = adb.Shell(serial, ShellCommand("ls").arg("-1").arg("/storage"), { .timeout = 10s, .cancel = st });
        if (r.ok()) {
            const auto ids = ParseStorageVolumes(r.out);
            for (size_t i = 0; i < ids.size(); ++i)
                vols.push_back({ "/storage/" + ids[i] + "/", ids.size() > 1 ? "SD card " + std::to_string(i + 1) : "SD card", true });
        }
    }
    // Capacity per volume (internal storage reports /data's numbers).
    for (auto& v : vols) {
        const auto r = adb.Shell(serial, ShellCommand("df").arg("-k").arg(v.root), { .timeout = 10s, .cancel = st });
        if (auto s = ParseDfK(r.out)) {
            v.totalBytes = s->total;
            v.usedBytes = s->used;
            v.freeBytes = s->free;
        }
    }
    return vols;
}

ListMethod FileScanner::Probe(const AdbClient& adb, std::string_view serial, const std::string& root, std::stop_token st)
{
    // 1. find -printf (GNU-style; newer toybox has it, toybox 0.7.x doesn't).
    {
        ShellCommand cmd("find");
        cmd.arg(root).arg("-maxdepth").arg("0").arg("-printf").arg(kPrintfFormat);
        const auto r = adb.Shell(serial, cmd, { .timeout = 10s, .cancel = st });
        if (r.ok() && ParseStatLine(Trim(r.out)))
            return ListMethod::FindPrintf;
    }
    // 2. stat -c with a literal TAB separator (toybox stat doesn't expand \t).
    {
        ShellCommand cmd("stat");
        cmd.arg("-c").arg(kStatFormat).arg(root);
        const auto r = adb.Shell(serial, cmd, { .timeout = 10s, .cancel = st });
        if (r.ok() && ParseStatLine(Trim(r.out)))
            return ListMethod::FindExecStat;
    }
    return ListMethod::None;
}

ScanOutcome FileScanner::Run(const AdbClient& adb, std::string_view serial, const ScanOptions& opts, const ScanSink& sink,
    std::stop_token st)
{
    ScanOutcome out;
    auto phase = [&](std::string p) {
        if (sink.onPhase)
            sink.onPhase(std::move(p));
    };

    phase("Finding storage volumes");
    const auto volumes = DiscoverVolumes(adb, serial, opts.includeSdCard, st);
    if (sink.onVolumes)
        sink.onVolumes(volumes);

    phase("Probing toybox");
    out.method = Probe(adb, serial, kInternalRoot, st);
    if (st.stop_requested()) {
        out.cancelled = true;
        return out;
    }
    if (out.method == ListMethod::None) {
        out.error = "This phone's find/stat tools don't support any listing format PhoneSweep knows.";
        out.errorDetail = "Tried: find -printf, stat -c";
        return out;
    }
    LogInfo("File listing method: {}", ToString(out.method));

    std::vector<FileEntry> batch;
    batch.reserve(kBatch);
    auto flush = [&] {
        if (!batch.empty() && sink.onBatch)
            sink.onBatch(std::move(batch));
        batch.clear();
        batch.reserve(kBatch);
    };

    for (size_t vi = 0; vi < volumes.size(); ++vi) {
        const auto& vol = volumes[vi];
        phase(std::format("Listing {}", vol.label));

        // The trailing slash matters: /sdcard is a symlink and find won't follow it.
        ShellCommand cmd("find");
        cmd.arg(vol.root).arg("-type").arg("f");
        if (out.method == ListMethod::FindPrintf)
            cmd.arg("-printf").arg(kPrintfFormat);
        else
            cmd.arg("-exec").arg("stat").arg("-c").arg(kStatFormat).arg("{}").arg("+");

        StatStreamParser parser([&](StatRecord&& rec) {
            if (rec.size < opts.minSizeBytes)
                return;
            FileEntry e;
            e.path = std::move(rec.path);
            e.size = rec.size;
            e.diskBytes = rec.diskBytes;
            e.mtime = rec.mtime;
            e.volume = static_cast<uint8_t>(vi);
            Classify(e, vol.root);
            batch.push_back(std::move(e));
            ++out.files;
            if (batch.size() >= kBatch)
                flush();
        });

        const auto r = adb.ShellStream(serial, cmd, [&](std::string_view line) { parser.Feed(line); },
            { .timeout = kListTimeout, .cancel = st });
        parser.Finish();
        flush();

        out.permissionDenied += CountLinesContaining(r.err, "Permission denied");
        out.unreadableNames += CountLinesContaining(r.err, "No such file or directory");

        if (r.status == AdbResult::Status::Cancelled || st.stop_requested()) {
            out.cancelled = true;
            return out;
        }
        if (r.status != AdbResult::Status::Ok) {
            out.error = r.status == AdbResult::Status::Timeout ? "Listing files took too long and was stopped."
                                                               : "Couldn't run adb to list files.";
            out.errorDetail = std::format("adb -s {} shell {}\n{}", serial, cmd.str(), r.err);
            return out;
        }
        // find exits 1 when some paths were unreadable; that's expected, not fatal.
        // Anything else that produced no output at all means the phone went away.
        if (r.exitCode != 0 && parser.Emitted() == 0) {
            const bool gone = r.err.find("no devices") != std::string::npos || r.err.find("not found") != std::string::npos
                || r.err.find("offline") != std::string::npos;
            out.error = gone ? "The phone disconnected during the scan." : std::format("Listing {} failed.", vol.label);
            out.errorDetail = std::format("adb -s {} shell {}\nexit {}\n{}", serial, cmd.str(), r.exitCode, r.err);
            if (vi == 0)
                return out; // internal storage is required; a broken SD card is not
            LogWarn("{}: {}", out.error, r.err);
            out.error.clear();
            out.errorDetail.clear();
        }
        if (parser.Glued())
            LogInfo("{}: {} file names contained line breaks", vol.label, parser.Glued());
    }

    if (out.permissionDenied)
        LogWarn("{} folders couldn't be read (permission denied)", out.permissionDenied);
    if (out.unreadableNames)
        LogWarn("{} files have names the phone itself can't open (broken encoding)", out.unreadableNames);

    // MediaStore: enrich MIME types. Best effort, never fatal.
    phase("Reading MediaStore");
    {
        ShellCommand cmd("content");
        cmd.arg("query").arg("--uri").arg("content://media/external/file").arg("--projection").arg("_data:mime_type");
        std::vector<MediaRow> rows;
        const auto r = adb.ShellStream(serial, cmd,
            [&](std::string_view line) {
                if (auto row = ParseMediaStoreRow(line))
                    rows.push_back(std::move(*row));
            },
            { .timeout = std::chrono::minutes(3), .cancel = st });
        if (st.stop_requested()) {
            out.cancelled = true;
            return out;
        }
        if (r.status == AdbResult::Status::Ok && !rows.empty()) {
            LogInfo("MediaStore: {} rows with MIME types", rows.size());
            if (sink.onMediaStore)
                sink.onMediaStore(std::move(rows));
        } else {
            LogWarn("MediaStore query unavailable (exit {}), using extensions only", r.exitCode);
        }
    }

    out.ok = true;
    return out;
}

} // namespace ps
