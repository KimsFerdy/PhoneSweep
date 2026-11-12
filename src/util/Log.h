#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ps {

enum class LogLevel : uint8_t { Info, Warn, Error, AdbCmd };

struct LogEntry {
    std::chrono::system_clock::time_point time;
    LogLevel level;
    std::string text;
};

// Thread-safe logger: appends to a session log file and keeps a bounded
// in-memory ring for the in-app Log panel.
class Log {
public:
    static Log& Get();

    void Open(const std::filesystem::path& logDir);
    void Write(LogLevel level, std::string text);

    // Copies entries with index >= fromIndex. Returns the next index to ask for.
    // Indices are monotonic across the whole session, so the UI can poll cheaply.
    uint64_t Snapshot(uint64_t fromIndex, std::vector<LogEntry>& out) const;
    std::filesystem::path FilePath() const;

private:
    Log() = default;

    static constexpr size_t kMaxEntries = 20000;

    mutable std::mutex m_mutex;
    std::deque<LogEntry> m_entries;
    uint64_t m_firstIndex = 0; // session index of m_entries.front()
    std::ofstream m_file;
    std::filesystem::path m_path;
};

const char* ToString(LogLevel level);

template <class... Args>
void LogInfo(std::format_string<Args...> fmt, Args&&... args)
{
    Log::Get().Write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void LogWarn(std::format_string<Args...> fmt, Args&&... args)
{
    Log::Get().Write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void LogError(std::format_string<Args...> fmt, Args&&... args)
{
    Log::Get().Write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace ps
