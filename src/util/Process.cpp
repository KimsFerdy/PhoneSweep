#include "util/Process.h"

#include "adb/ShellCommand.h" // QuoteWindowsArg
#include "util/Log.h"
#include "util/Utf8.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <vector>

namespace ps {

namespace {

struct HandleCloser {
    void operator()(HANDLE h) const
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// Reads whatever is currently available on a pipe without blocking.
bool DrainPipe(HANDLE pipe, std::string& sink)
{
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
            return false;
        if (avail == 0)
            return true;
        char buf[64 * 1024];
        DWORD got = 0;
        if (!ReadFile(pipe, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr))
            return false;
        sink.append(buf, got);
    }
}

std::string JoinForLog(std::string_view name, std::span<const std::string> args)
{
    std::string s(name);
    for (const auto& a : args) {
        s.push_back(' ');
        s += a;
    }
    return s;
}

} // namespace

const char* ToString(ProcessResult::Status s)
{
    switch (s) {
    case ProcessResult::Status::Ok: return "ok";
    case ProcessResult::Status::Timeout: return "timeout";
    case ProcessResult::Status::Cancelled: return "cancelled";
    case ProcessResult::Status::SpawnFailed: return "spawn failed";
    }
    return "?";
}

std::filesystem::path ExeDir()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

namespace {

// PATH as currently stored in the registry (User + Machine), expanded. Programs
// installed after we (or our parent shell) started aren't in our own PATH yet.
std::vector<std::wstring> RegistryPath()
{
    std::vector<std::wstring> dirs;
    auto read = [&](HKEY root, const wchar_t* sub) {
        DWORD size = 0;
        if (RegGetValueW(root, sub, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr, &size) != ERROR_SUCCESS)
            return;
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, sub, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw.data(), &size) != ERROR_SUCCESS)
            return;
        raw.resize(wcsnlen(raw.c_str(), raw.size()));
        wchar_t expanded[32767];
        const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, 32767);
        const std::wstring all = n ? std::wstring(expanded) : raw;
        size_t s = 0;
        while (s <= all.size()) {
            const auto e = all.find(L';', s);
            auto dir = all.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
            if (!dir.empty())
                dirs.push_back(std::move(dir));
            if (e == std::wstring::npos)
                break;
            s = e + 1;
        }
    };
    read(HKEY_CURRENT_USER, L"Environment");
    read(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    return dirs;
}

} // namespace

std::optional<std::filesystem::path> FindExecutable(const wchar_t* fileName)
{
    std::error_code ec;
    if (const auto local = ExeDir() / fileName; std::filesystem::is_regular_file(local, ec))
        return local;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, fileName, nullptr, MAX_PATH, found, nullptr) > 0)
        return std::filesystem::path(found);
    for (const auto& dir : RegistryPath())
        if (const auto p = std::filesystem::path(dir) / fileName; std::filesystem::is_regular_file(p, ec))
            return p;
    // winget: Links\<exe> aliases, or Packages\<id>\<version>\bin\<exe> (e.g. Gyan.FFmpeg).
    if (wchar_t local[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
        const auto wg = std::filesystem::path(local) / L"Microsoft" / L"WinGet";
        if (const auto p = wg / L"Links" / fileName; std::filesystem::is_regular_file(p, ec))
            return p;
        for (std::filesystem::directory_iterator pkg(wg / L"Packages", ec), end; !ec && pkg != end; pkg.increment(ec)) {
            std::error_code ec2; // separate, so one unreadable package doesn't stop the outer walk
            for (std::filesystem::directory_iterator ver(pkg->path(), ec2), vend; !ec2 && ver != vend; ver.increment(ec2))
                if (const auto p = ver->path() / L"bin" / fileName; std::filesystem::is_regular_file(p, ec2))
                    return p;
        }
    }
    return std::nullopt;
}

ProcessResult RunProcess(const std::filesystem::path& exe, std::span<const std::string> args, const ProcessOptions& opts,
    const std::function<void(std::string_view)>& onStdout)
{
    ProcessResult result;
    const std::string name = opts.logName.empty() ? Narrow(exe.filename().wstring()) : opts.logName;
    const auto t0 = std::chrono::steady_clock::now();
    const auto finish = [&](ProcessResult::Status st) {
        result.status = st;
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Log::Get().Write(st == ProcessResult::Status::Ok ? LogLevel::AdbCmd : LogLevel::Warn,
            std::format("{} -> exit {} ({}, {} ms){}", JoinForLog(name, args), result.exitCode, ToString(st), result.elapsed.count(),
                result.err.empty() ? "" : " stderr: " + result.err.substr(0, 300)));
        return result;
    };

    // Pipes: parent keeps the read ends (non-inheritable), child gets the write ends.
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 1 << 20) || !CreatePipe(&errRead, &errWrite, &sa, 64 << 10)) {
        result.err = "CreatePipe failed";
        return finish(ProcessResult::Status::SpawnFailed);
    }
    UniqueHandle hOutRead(outRead), hOutWrite(outWrite), hErrRead(errRead), hErrWrite(errWrite);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    UniqueHandle hNul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));

    // Only these three handles may be inherited. Without this, two jobs spawning
    // at once would leak each other's pipe write-ends and never see EOF.
    HANDLE inherit[] = { hNul.get(), outWrite, errWrite };
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<std::byte> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)
        || !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        result.err = "proc attribute setup failed";
        return finish(ProcessResult::Status::SpawnFailed);
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attrGuard{ attrs };

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = hNul.get();
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError = errWrite;
    si.lpAttributeList = attrs;

    std::wstring cmdLine = QuoteWindowsArg(exe.wstring());
    for (const auto& a : args) {
        cmdLine.push_back(L' ');
        cmdLine += QuoteWindowsArg(Widen(a));
    }

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si.StartupInfo, &pi)) {
        result.err = std::format("CreateProcessW failed (error {})", GetLastError());
        return finish(ProcessResult::Status::SpawnFailed);
    }
    UniqueHandle hProc(pi.hProcess), hThread(pi.hThread);
    // Close our copies of the write ends so the pipes break when the child exits.
    hOutWrite.reset();
    hErrWrite.reset();

    auto deliver = [&](std::string& chunk) {
        if (chunk.empty())
            return;
        if (onStdout)
            onStdout(chunk);
        else
            result.out.append(chunk);
    };

    // Poll instead of blocking on EOF: if adb has to start its server, the
    // daemon can outlive the client and keep the pipe open forever.
    std::string chunk;
    const auto deadline = t0 + opts.timeout;
    auto status = ProcessResult::Status::Ok;
    for (;;) {
        chunk.clear();
        DrainPipe(outRead, chunk);
        const bool gotData = !chunk.empty();
        deliver(chunk);
        DrainPipe(errRead, result.err);

        const DWORD w = WaitForSingleObject(pi.hProcess, gotData ? 0 : 5);
        if (w == WAIT_OBJECT_0)
            break;
        if (opts.cancel.stop_requested()) {
            status = ProcessResult::Status::Cancelled;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            status = ProcessResult::Status::Timeout;
            break;
        }
    }

    if (status != ProcessResult::Status::Ok) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }

    // Final drain of anything written just before exit.
    chunk.clear();
    DrainPipe(outRead, chunk);
    deliver(chunk);
    DrainPipe(errRead, result.err);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    while (!result.err.empty() && (result.err.back() == '\n' || result.err.back() == '\r'))
        result.err.pop_back();
    return finish(status);
}

} // namespace ps
