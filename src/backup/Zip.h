#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

// Minimal ZIP support for backups: "stored" entries (photos/videos are already
// compressed, so deflate would only cost time), UTF-8 names, ZIP64 when an entry,
// offset or count needs it. Readable by Windows Explorer, 7-Zip, bsdtar, etc.

uint32_t Crc32(std::span<const uint8_t> data, uint32_t crc = 0);

class ZipWriter {
public:
    ZipWriter() = default;
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool Open(const std::filesystem::path& file, std::string* error);
    // Streams `source` into the archive as `name` ("DCIM/Camera/a.jpg").
    bool AddFile(const std::string& name, const std::filesystem::path& source, int64_t mtime, std::stop_token st,
        const std::function<void(uint64_t bytes)>& onBytes, std::string* error);
    bool AddBytes(const std::string& name, std::string_view data, int64_t mtime, std::string* error);
    // Writes the central directory. The archive is only valid after this.
    bool Close(std::string* error);

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0;
        uint64_t size = 0, offset = 0;
        uint16_t dosTime = 0, dosDate = 0;
    };
    bool WriteHeader(Entry& e, std::string* error);
    bool PatchCrc(const Entry& e, std::string* error);

    std::FILE* m_file = nullptr;
    std::vector<Entry> m_entries;
};

struct ZipEntry {
    std::string name;
    uint64_t size = 0, compressedSize = 0, localOffset = 0;
    uint32_t crc = 0;
    uint16_t method = 0; // 0 = stored (the only one we read)
};

class ZipReader {
public:
    ZipReader() = default;
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool Open(const std::filesystem::path& file, std::string* error);
    const std::vector<ZipEntry>& Entries() const { return m_entries; }
    const ZipEntry* Find(std::string_view name) const;

    // Streams an entry's bytes to `sink` and checks its CRC. false on any error.
    bool Read(const ZipEntry& e, const std::function<void(std::span<const uint8_t>)>& sink, std::stop_token st, std::string* error);
    std::optional<std::string> ReadAll(const ZipEntry& e, std::string* error); // small entries (manifest)
    bool Extract(const ZipEntry& e, const std::filesystem::path& to, std::stop_token st, std::string* error);

private:
    std::FILE* m_file = nullptr;
    std::vector<ZipEntry> m_entries;
};

} // namespace ps
