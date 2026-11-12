#include "transfer/PhoneIO.h"

#include "adb/AdbClient.h"
#include "transfer/LocalPath.h"
#include "util/Log.h"
#include "util/Md5.h"
#include "util/Strings.h"
#include "util/Utf8.h"

#include <fstream>

namespace ps {

PullResult PullToFile(const AdbClient& adb, const std::string& serial, const std::string& devicePath, const std::filesystem::path& local,
    std::stop_token st, const std::function<void(uint64_t)>& onBytes)
{
    PullResult res;
    std::error_code ec;
    std::filesystem::create_directories(LongPath(local.parent_path()), ec);
    std::ofstream out(LongPath(local), std::ios::binary | std::ios::trunc);
    if (!out) {
        res.error = "Couldn't create a temp file on the PC";
        return res;
    }
    Md5 md5;
    bool writeFailed = false;
    const std::string args[] = { "-s", serial, "exec-out", ShellCommand("cat").arg(devicePath).str() };
    const auto r = RunProcess(adb.ExePath(), args, { .timeout = std::chrono::minutes(60), .cancel = st, .logName = "adb" },
        [&](std::string_view data) {
            if (writeFailed)
                return;
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!out) {
                writeFailed = true;
                return;
            }
            md5.Update(data);
            res.bytes += data.size();
            if (onBytes)
                onBytes(res.bytes);
        });
    out.close();
    if (writeFailed) {
        res.error = "Writing to the PC failed (disk full?)";
        return res;
    }
    if (!r.ok()) {
        res.deviceGone = r.status != ProcessResult::Status::Ok || r.err.find("device") != std::string::npos;
        res.error = r.err.empty() ? std::string("Copy from the phone failed") : r.err;
        return res;
    }
    res.md5 = md5.HexFinal();
    res.ok = true;
    return res;
}

std::optional<std::string> PhoneMd5(const AdbClient& adb, const std::string& serial, const std::string& path, std::stop_token st)
{
    const auto r = adb.Shell(serial, ShellCommand("md5sum").arg(path), { .timeout = std::chrono::minutes(30), .cancel = st });
    if (!r.ok())
        return std::nullopt;
    const auto t = SplitWhitespace(r.out);
    if (t.empty() || t[0].size() != 32)
        return std::nullopt;
    return std::string(t[0]);
}

std::string PushVerified(const AdbClient& adb, const std::string& serial, const std::filesystem::path& local, const std::string& devicePath,
    const std::string& localMd5, int64_t mtime, std::stop_token st)
{
    const auto slash = devicePath.rfind('/');
    const std::string dir = devicePath.substr(0, slash);
    const std::string tmp = dir + "/.phonesweep_tmp_" + devicePath.substr(slash + 1);
    adb.Shell(serial, ShellCommand("mkdir").arg("-p").arg(dir), { .timeout = 30s, .cancel = st });

    const auto push = adb.Push(serial, local, tmp, { .timeout = std::chrono::minutes(60), .cancel = st });
    if (!push.ok()) {
        adb.Shell(serial, ShellCommand("rm").arg("-f").arg(tmp), { .timeout = 30s });
        return push.err.empty() ? std::string("Upload to the phone failed") : push.err;
    }
    // Verify what landed on the phone before it replaces anything.
    const auto remote = PhoneMd5(adb, serial, tmp, st);
    if (!remote || *remote != localMd5) {
        adb.Shell(serial, ShellCommand("rm").arg("-f").arg(tmp), { .timeout = 30s });
        return "Checksum mismatch after upload (nothing was replaced)";
    }
    ShellCommand mv("mv");
    mv.arg("-f").arg(tmp).arg(devicePath);
    if (mtime > 0) {
        // toybox touch has no "-d @epoch". date and touch must agree on the timezone (they don't by
        // default: measured 3 h off on an Android 9 Samsung), so pin both to UTC.
        mv.raw("&& TZ=UTC touch -m -t \"$(TZ=UTC date -d @" + std::to_string(mtime) + " +%Y%m%d%H%M.%S)\"").arg(devicePath);
    }
    const auto r = adb.Shell(serial, mv, { .timeout = 60s, .cancel = st });
    if (!r.ok()) {
        adb.Shell(serial, ShellCommand("rm").arg("-f").arg(tmp), { .timeout = 30s });
        return r.err.empty() ? std::string("Couldn't move the file into place") : r.err;
    }
    return {};
}

void MediaScan(const AdbClient& adb, const std::string& serial, const std::vector<std::string>& paths, std::stop_token st)
{
    for (size_t i = 0; i < paths.size() && !st.stop_requested(); i += 50) {
        ShellCommand cmd("for f in");
        for (size_t k = i; k < std::min(paths.size(), i + 50); ++k)
            cmd.arg(paths[k]);
        cmd.raw("; do am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d \"file://$f\" >/dev/null 2>&1; done");
        adb.Shell(serial, cmd, { .timeout = std::chrono::minutes(5), .cancel = st });
    }
}

} // namespace ps
