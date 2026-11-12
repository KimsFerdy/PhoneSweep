#include "adb/AdbClient.h"

#include "util/Strings.h"
#include "util/Utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ps {

std::optional<std::filesystem::path> AdbClient::Locate()
{
    std::error_code ec;
    const auto dir = ExeDir();
    for (const auto& candidate : { dir / L"adb.exe", dir / L"platform-tools" / L"adb.exe",
             std::filesystem::current_path(ec) / L"platform-tools" / L"adb.exe" }) {
        if (std::filesystem::is_regular_file(candidate, ec))
            return candidate;
    }

    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) > 0)
        return std::filesystem::path(found);

    // Not in the build spec's list, but it's where Android Studio puts it and
    // costs nothing to check.
    if (wchar_t local[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
        auto sdk = std::filesystem::path(local) / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe";
        if (std::filesystem::is_regular_file(sdk, ec))
            return sdk;
    }
    return std::nullopt;
}

AdbClient::AdbClient(std::filesystem::path adbExe)
    : m_exe(std::move(adbExe))
{
}

AdbResult AdbClient::Exec(std::span<const std::string> args, const RunOptions& opts,
    const std::function<void(std::string_view)>& onStdout) const
{
    return RunProcess(m_exe, args, { .timeout = opts.timeout, .cancel = opts.cancel, .logName = "adb" }, onStdout);
}

AdbResult AdbClient::Run(std::span<const std::string> args, const RunOptions& opts) const
{
    return Exec(args, opts, {});
}

AdbResult AdbClient::Run(std::initializer_list<std::string> args, const RunOptions& opts) const
{
    return Run(std::span<const std::string>(args.begin(), args.size()), opts);
}

AdbResult AdbClient::Shell(std::string_view serial, const ShellCommand& cmd, const RunOptions& opts) const
{
    const std::string args[] = { "-s", std::string(serial), "shell", cmd.str() };
    return Run(args, opts);
}

AdbResult AdbClient::ExecOut(std::string_view serial, const ShellCommand& cmd, const RunOptions& opts) const
{
    const std::string args[] = { "-s", std::string(serial), "exec-out", cmd.str() };
    return Run(args, opts);
}

AdbResult AdbClient::ShellStream(std::string_view serial, const ShellCommand& cmd,
    const std::function<void(std::string_view)>& onLine, const RunOptions& opts) const
{
    const std::string args[] = { "-s", std::string(serial), "shell", cmd.str() };
    std::string pending;
    auto emit = [&](std::string_view line) {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        onLine(line);
    };
    AdbResult r = Exec(args, opts, [&](std::string_view chunk) {
        pending.append(chunk);
        size_t start = 0;
        for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1)
            emit(std::string_view(pending).substr(start, nl - start));
        pending.erase(0, start);
    });
    if (!pending.empty())
        emit(pending);
    return r;
}

AdbResult AdbClient::Pull(std::string_view serial, std::string_view remote, const std::filesystem::path& local,
    const RunOptions& opts) const
{
    const std::string args[] = { "-s", std::string(serial), "pull", "-a", std::string(remote), Narrow(local.wstring()) };
    return Run(args, opts);
}

AdbResult AdbClient::Push(std::string_view serial, const std::filesystem::path& local, std::string_view remote,
    const RunOptions& opts) const
{
    const std::string args[] = { "-s", std::string(serial), "push", Narrow(local.wstring()), std::string(remote) };
    return Run(args, opts);
}

std::string AdbClient::Version() const
{
    const AdbResult r = Run({ "version" }, { .timeout = 10s });
    if (!r.ok())
        return {};
    // "Android Debug Bridge version 1.0.41\nVersion 37.0.1-15733141\n..."
    std::string proto, ver;
    if (auto p = r.out.find("version "); p != std::string::npos)
        proto = std::string(Trim(std::string_view(r.out).substr(p + 8, r.out.find('\n', p) - p - 8)));
    if (auto p = r.out.find("\nVersion "); p != std::string::npos)
        ver = std::string(Trim(std::string_view(r.out).substr(p + 9, r.out.find('\n', p + 1) - p - 9)));
    return ver.empty() ? proto : ver;
}

} // namespace ps
