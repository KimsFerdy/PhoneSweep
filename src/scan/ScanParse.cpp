#include "scan/ScanParse.h"

#include "util/Strings.h"

namespace ps {

std::optional<StatRecord> ParseStatLine(std::string_view line)
{
    // Collect leading numeric fields until the path (first field starting with '/').
    std::vector<std::string_view> nums;
    size_t pos = 0;
    while (nums.size() < 4) {
        const auto tab = line.find('\t', pos);
        if (tab == std::string_view::npos || tab == pos)
            return std::nullopt;
        nums.push_back(line.substr(pos, tab - pos));
        pos = tab + 1;
        if (pos < line.size() && line[pos] == '/')
            break;
    }
    if (pos >= line.size() || line[pos] != '/' || nums.size() < 2)
        return std::nullopt;

    auto mtimeStr = nums.back();
    if (const auto dot = mtimeStr.find('.'); dot != std::string_view::npos) {
        // Fraction must be digits too, or this isn't a stat line.
        for (char c : mtimeStr.substr(dot + 1))
            if (c < '0' || c > '9')
                return std::nullopt;
        mtimeStr = mtimeStr.substr(0, dot);
    }
    const auto size = ParseInt<uint64_t>(nums[0]);
    const auto mtime = ParseInt<int64_t>(mtimeStr);
    if (!size || !mtime)
        return std::nullopt;

    StatRecord r{ *size, 0, *mtime, std::string(line.substr(pos)) };
    if (nums.size() == 3) { // size, KB, mtime
        const auto kb = ParseInt<uint64_t>(nums[1]);
        if (!kb)
            return std::nullopt;
        r.diskBytes = *kb * 1024;
    } else if (nums.size() == 4) { // size, blocks, blockSize, mtime
        const auto blocks = ParseInt<uint64_t>(nums[1]);
        const auto bsize = ParseInt<uint64_t>(nums[2]);
        if (!blocks || !bsize)
            return std::nullopt;
        r.diskBytes = *blocks * *bsize;
    }
    return r;
}

void StatStreamParser::Feed(std::string_view line)
{
    if (auto rec = ParseStatLine(line)) {
        if (m_pending) {
            m_sink(std::move(*m_pending));
            ++m_emitted;
        }
        m_pending = std::move(rec);
        return;
    }
    if (m_pending) {
        m_pending->path.push_back('\n');
        m_pending->path.append(line);
        ++m_glued;
    } else if (!line.empty()) {
        ++m_orphans;
    }
}

void StatStreamParser::Finish()
{
    if (m_pending) {
        m_sink(std::move(*m_pending));
        ++m_emitted;
        m_pending.reset();
    }
}

std::optional<MediaRow> ParseMediaStoreRow(std::string_view line)
{
    if (!line.starts_with("Row: "))
        return std::nullopt;
    const auto dataPos = line.find(" _data=");
    if (dataPos == std::string_view::npos)
        return std::nullopt;
    // Paths may contain ", " so anchor on the LAST mime_type field.
    const auto mimePos = line.rfind(", mime_type=");
    if (mimePos == std::string_view::npos || mimePos < dataPos)
        return std::nullopt;
    MediaRow row;
    row.path = std::string(line.substr(dataPos + 7, mimePos - dataPos - 7));
    row.mime = std::string(Trim(line.substr(mimePos + 12)));
    if (row.path.empty() || row.path == "NULL" || row.mime.empty() || row.mime == "NULL")
        return std::nullopt;
    return row;
}

std::vector<std::string> ParseStorageVolumes(std::string_view lsOutput)
{
    std::vector<std::string> out;
    ForEachLine(lsOutput, [&](std::string_view name) {
        name = Trim(name);
        // FAT/exFAT volume ids look like "0000-0000" / "1A2B-3C4D".
        if (name.size() != 9 || name[4] != '-')
            return;
        for (size_t i = 0; i < 9; ++i) {
            if (i == 4)
                continue;
            const char c = name[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
                return;
        }
        out.emplace_back(name);
    });
    return out;
}

} // namespace ps
