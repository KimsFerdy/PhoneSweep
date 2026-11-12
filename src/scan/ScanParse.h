#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

struct StatRecord {
    uint64_t size = 0;      // apparent size (st_size)
    uint64_t diskBytes = 0; // space actually allocated (0 = unknown). Sparse files: diskBytes << size
    int64_t mtime = 0;
    std::string path;
};

// One stat line. Numeric tab-separated fields come first, then the path:
//   size, mtime                    stat -c '%s\t%Y\t%n'          (old format)
//   size, allocKB, mtime           find -printf '%s\t%k\t%T@\t%p'
//   size, blocks, blockSize, mtime stat -c '%s\t%b\t%B\t%Y\t%n'
// mtime may be fractional ("1729902815.1234567"); the fraction is dropped.
// The path starts at the first field beginning with '/' (it may contain tabs).
std::optional<StatRecord> ParseStatLine(std::string_view line);

// Feeds raw output lines and emits records. A file name containing '\n' arrives
// as a valid line followed by line(s) that don't parse; those are glued back on.
class StatStreamParser {
public:
    using Sink = std::function<void(StatRecord&&)>;
    explicit StatStreamParser(Sink sink) : m_sink(std::move(sink)) {}

    void Feed(std::string_view line);
    void Finish();

    size_t Emitted() const { return m_emitted; }
    size_t Glued() const { return m_glued; }
    size_t Orphans() const { return m_orphans; } // unparseable lines with nothing to glue to

private:
    Sink m_sink;
    std::optional<StatRecord> m_pending;
    size_t m_emitted = 0, m_glued = 0, m_orphans = 0;
};

// `content query ... --projection _data:mime_type` row:
//   "Row: 12 _data=/storage/emulated/0/DCIM/a, b.jpg, mime_type=image/jpeg"
// Returns {path, mime}; nullopt for header/garbage or NULL mime.
struct MediaRow {
    std::string path;
    std::string mime;
};
std::optional<MediaRow> ParseMediaStoreRow(std::string_view line);

// `ls -1 /storage` -> SD card volume ids ("0000-0000", "1A2B-3C4D").
std::vector<std::string> ParseStorageVolumes(std::string_view lsOutput);

} // namespace ps
