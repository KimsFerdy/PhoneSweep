#pragma once
#include "adb/ShellCommand.h"
#include "util/Process.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

using AdbResult = ProcessResult;

struct RunOptions {
    std::chrono::milliseconds timeout = 30s;
    std::stop_token cancel;
};

// Runs Google's adb.exe as a child process. Stateless apart from the exe path,
// so one instance can be shared across worker threads.
class AdbClient {
public:
    // Next to our exe -> ./platform-tools -> PATH -> Android SDK default.
    static std::optional<std::filesystem::path> Locate();

    explicit AdbClient(std::filesystem::path adbExe);

    const std::filesystem::path& ExePath() const { return m_exe; }

    // Raw adb argv (without "adb" itself). Each element is quoted for CreateProcessW.
    AdbResult Run(std::span<const std::string> args, const RunOptions& opts = {}) const;
    AdbResult Run(std::initializer_list<std::string> args, const RunOptions& opts = {}) const;

    // adb -s <serial> shell <cmd>
    AdbResult Shell(std::string_view serial, const ShellCommand& cmd, const RunOptions& opts = {}) const;

    // Same, but delivers stdout line by line (trailing \r stripped) on the calling
    // thread as it arrives. Used for find output with 50k+ lines.
    AdbResult ShellStream(std::string_view serial, const ShellCommand& cmd,
        const std::function<void(std::string_view line)>& onLine, const RunOptions& opts = {}) const;

    // adb -s <serial> exec-out <cmd>: binary-safe stdout (no pty, no CRLF mangling).
    // result.out holds the raw bytes. Use for `cat`/`head -c` of images.
    AdbResult ExecOut(std::string_view serial, const ShellCommand& cmd, const RunOptions& opts = {}) const;

    AdbResult Pull(std::string_view serial, std::string_view remote, const std::filesystem::path& local,
        const RunOptions& opts = {}) const;
    AdbResult Push(std::string_view serial, const std::filesystem::path& local, std::string_view remote,
        const RunOptions& opts = {}) const;

    // "37.0.1-15733141" style string, empty on failure.
    std::string Version() const;

private:
    AdbResult Exec(std::span<const std::string> args, const RunOptions& opts,
        const std::function<void(std::string_view)>& onStdout) const;

    std::filesystem::path m_exe;
};

} // namespace ps
