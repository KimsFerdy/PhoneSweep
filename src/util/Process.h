#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace ps {

using namespace std::chrono_literals;

struct ProcessResult {
    enum class Status { Ok, Timeout, Cancelled, SpawnFailed };

    Status status = Status::SpawnFailed;
    int exitCode = -1;
    std::string out; // raw stdout bytes (empty when streamed)
    std::string err; // stderr, UTF-8
    std::chrono::milliseconds elapsed{};

    bool ok() const { return status == Status::Ok && exitCode == 0; }
};

const char* ToString(ProcessResult::Status s);

struct ProcessOptions {
    std::chrono::milliseconds timeout = 30s;
    std::stop_token cancel;
    std::string logName; // shown in the log instead of the exe path ("adb", "ffmpeg")
};

// Spawns `exe args...` with CreateProcessW (each arg quoted for the MSVC CRT),
// no console window, stdin = NUL. Only the child's pipe handles are inherited.
// stdout is delivered raw (binary-safe) to onStdout as it arrives; if onStdout
// is empty it is collected into ProcessResult::out. Every run is logged.
ProcessResult RunProcess(const std::filesystem::path& exe, std::span<const std::string> args, const ProcessOptions& opts,
    const std::function<void(std::string_view chunk)>& onStdout = {});

std::filesystem::path ExeDir();
// Looks next to our exe, then on PATH.
std::optional<std::filesystem::path> FindExecutable(const wchar_t* fileName);

} // namespace ps
