#include "util/Log.h"

#include <ctime>

namespace ps {

const char* ToString(LogLevel level)
{
    switch (level) {
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::AdbCmd: return "ADB";
    }
    return "?";
}

Log& Log::Get()
{
    static Log instance;
    return instance;
}

void Log::Open(const std::filesystem::path& logDir)
{
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[64];
    std::strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S.log", &tm);

    std::lock_guard lock(m_mutex);
    m_path = logDir / name;
    m_file.open(m_path, std::ios::out | std::ios::binary);
}

void Log::Write(LogLevel level, std::string text)
{
    const auto now = std::chrono::system_clock::now();
    std::lock_guard lock(m_mutex);

    if (m_file.is_open()) {
        const auto local = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::milliseconds>(now));
        m_file << std::format("{:%H:%M:%S} [{}] {}\n", local, ToString(level), text);
        m_file.flush();
    }

    m_entries.push_back({ now, level, std::move(text) });
    if (m_entries.size() > kMaxEntries) {
        m_entries.pop_front();
        ++m_firstIndex;
    }
}

uint64_t Log::Snapshot(uint64_t fromIndex, std::vector<LogEntry>& out) const
{
    std::lock_guard lock(m_mutex);
    const uint64_t end = m_firstIndex + m_entries.size();
    uint64_t i = std::max(fromIndex, m_firstIndex);
    for (; i < end; ++i)
        out.push_back(m_entries[static_cast<size_t>(i - m_firstIndex)]);
    return end;
}

std::filesystem::path Log::FilePath() const
{
    std::lock_guard lock(m_mutex);
    return m_path;
}

} // namespace ps
