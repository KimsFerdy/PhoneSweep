#pragma once
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

inline std::string_view Trim(std::string_view s)
{
    while (!s.empty() && IsSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

// Calls fn for each line (without '\n', trailing '\r' stripped). Views point into text.
template <class Fn>
void ForEachLine(std::string_view text, Fn&& fn)
{
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        fn(line);
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
}

inline std::vector<std::string_view> SplitWhitespace(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpace(s[i]))
            ++i;
        const size_t start = i;
        while (i < s.size() && !IsSpace(s[i]))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

template <class T>
std::optional<T> ParseInt(std::string_view s)
{
    T value{};
    const auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{} || ptr != end || s.empty())
        return std::nullopt;
    return value;
}

// "1.2 GB" style, decimal (1000) or binary (1024) units.
inline std::string FormatBytes(uint64_t bytes, bool binary = false)
{
    const double base = binary ? 1024.0 : 1000.0;
    static constexpr const char* kDec[] = { "B", "KB", "MB", "GB", "TB" };
    static constexpr const char* kBin[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= base && unit < 4) {
        v /= base;
        ++unit;
    }
    const char* suffix = binary ? kBin[unit] : kDec[unit];
    if (unit == 0)
        return std::format("{} {}", bytes, suffix);
    return v >= 100 ? std::format("{:.0f} {}", v, suffix) : std::format("{:.1f} {}", v, suffix);
}

} // namespace ps
