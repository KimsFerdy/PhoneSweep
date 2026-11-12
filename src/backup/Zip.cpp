#include "backup/Zip.h"

#include "transfer/LocalPath.h"

#include <array>
#include <ctime>
#include <format>

namespace ps {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr uint32_t kEnd64Sig = 0x06064b50, kLocator64Sig = 0x07064b50;
constexpr uint32_t k32 = 0xFFFFFFFFu;
constexpr uint16_t kUtf8Flag = 0x0800;
constexpr size_t kChunk = 1 << 20;

// Slice-by-8 CRC-32 (IEEE), ~1.5 GB/s: zipping must not be slower than the disk.
const std::array<std::array<uint32_t, 256>, 8>& CrcTables()
{
    static const auto tables = [] {
        std::array<std::array<uint32_t, 256>, 8> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        return t;
    }();
    return tables;
}

void Put16(std::string& b, uint16_t v)
{
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>(v >> 8));
}
void Put32(std::string& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void Put64(std::string& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t Get64(const uint8_t* p) { return static_cast<uint64_t>(Get32(p)) | (static_cast<uint64_t>(Get32(p + 4)) << 32); }

void DosTime(int64_t mtime, uint16_t& time, uint16_t& date)
{
    std::tm tm{};
    const std::time_t t = static_cast<std::time_t>(mtime > 0 ? mtime : std::time(nullptr));
    if (localtime_s(&tm, &t) != 0 || tm.tm_year < 80) {
        time = 0;
        date = (1 << 5) | 1; // 1980-01-01
        return;
    }
    time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    date = static_cast<uint16_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

bool WriteAll(std::FILE* f, const void* data, size_t n)
{
    return n == 0 || std::fwrite(data, 1, n, f) == n;
}

bool Fail(std::string* error, std::string msg)
{
    if (error)
        *error = std::move(msg);
    return false;
}

} // namespace

uint32_t Crc32(std::span<const uint8_t> data, uint32_t crc)
{
    const auto& t = CrcTables();
    crc = ~crc;
    const uint8_t* p = data.data();
    size_t n = data.size();
    while (n >= 8) {
        const uint32_t a = crc ^ Get32(p);
        const uint32_t b = Get32(p + 4);
        crc = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^ t[3][b & 0xFF] ^ t[2][(b >> 8) & 0xFF]
            ^ t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--)
        crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---- Writer ------------------------------------------------------------------------

ZipWriter::~ZipWriter()
{
    if (m_file)
        std::fclose(m_file);
}

bool ZipWriter::Open(const std::filesystem::path& file, std::string* error)
{
    if (_wfopen_s(&m_file, LongPath(file).c_str(), L"wb") != 0 || !m_file)
        return Fail(error, "can't create " + file.filename().string());
    std::setvbuf(m_file, nullptr, _IOFBF, kChunk);
    return true;
}

bool ZipWriter::WriteHeader(Entry& e, std::string* error)
{
    e.offset = static_cast<uint64_t>(_ftelli64(m_file));
    // Always a ZIP64 extra in the local header: sizes are known up front, but this keeps
    // the header length fixed so the CRC can be patched in place afterwards.
    std::string h;
    Put32(h, kLocalSig);
    Put16(h, 45);
    Put16(h, kUtf8Flag);
    Put16(h, 0); // stored
    Put16(h, e.dosTime);
    Put16(h, e.dosDate);
    Put32(h, 0); // CRC, patched later
    Put32(h, k32);
    Put32(h, k32);
    Put16(h, static_cast<uint16_t>(e.name.size()));
    Put16(h, 20);
    h += e.name;
    Put16(h, 0x0001);
    Put16(h, 16);
    Put64(h, e.size);
    Put64(h, e.size);
    if (!WriteAll(m_file, h.data(), h.size()))
        return Fail(error, "write failed (disk full?)");
    return true;
}

bool ZipWriter::PatchCrc(const Entry& e, std::string* error)
{
    const auto end = _ftelli64(m_file);
    std::string c;
    Put32(c, e.crc);
    if (_fseeki64(m_file, static_cast<int64_t>(e.offset) + 14, SEEK_SET) != 0 || !WriteAll(m_file, c.data(), 4) || _fseeki64(m_file, end, SEEK_SET) != 0)
        return Fail(error, "write failed (disk full?)");
    return true;
}

bool ZipWriter::AddFile(const std::string& name, const std::filesystem::path& source, int64_t mtime, std::stop_token st,
    const std::function<void(uint64_t)>& onBytes, std::string* error)
{
    if (!m_file)
        return Fail(error, "archive not open");
    std::FILE* in = nullptr;
    if (_wfopen_s(&in, LongPath(source).c_str(), L"rb") != 0 || !in)
        return Fail(error, "can't read " + source.filename().string());
    std::error_code ec;
    Entry e;
    e.name = name;
    e.size = std::filesystem::file_size(LongPath(source), ec);
    DosTime(mtime, e.dosTime, e.dosDate);
    if (ec || !WriteHeader(e, error)) {
        std::fclose(in);
        return ec ? Fail(error, "can't read " + source.filename().string()) : false;
    }
    std::vector<uint8_t> buf(kChunk);
    uint64_t total = 0;
    uint32_t crc = 0;
    while (true) {
        if (st.stop_requested()) {
            std::fclose(in);
            return Fail(error, "cancelled");
        }
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0)
            break;
        crc = Crc32({ buf.data(), n }, crc);
        if (!WriteAll(m_file, buf.data(), n)) {
            std::fclose(in);
            return Fail(error, "write failed (disk full?)");
        }
        total += n;
        if (onBytes)
            onBytes(n);
    }
    const bool readError = std::ferror(in) != 0;
    std::fclose(in);
    if (readError || total != e.size)
        return Fail(error, source.filename().string() + " changed while it was being zipped");
    e.crc = crc;
    if (!PatchCrc(e, error))
        return false;
    m_entries.push_back(std::move(e));
    return true;
}

bool ZipWriter::AddBytes(const std::string& name, std::string_view data, int64_t mtime, std::string* error)
{
    if (!m_file)
        return Fail(error, "archive not open");
    Entry e;
    e.name = name;
    e.size = data.size();
    e.crc = Crc32({ reinterpret_cast<const uint8_t*>(data.data()), data.size() });
    DosTime(mtime, e.dosTime, e.dosDate);
    if (!WriteHeader(e, error))
        return false;
    if (!WriteAll(m_file, data.data(), data.size()) || !PatchCrc(e, error))
        return Fail(error, "write failed (disk full?)");
    m_entries.push_back(std::move(e));
    return true;
}

bool ZipWriter::Close(std::string* error)
{
    if (!m_file)
        return Fail(error, "archive not open");
    const uint64_t cdOffset = static_cast<uint64_t>(_ftelli64(m_file));
    std::string cd;
    for (const auto& e : m_entries) {
        const bool big = e.size >= k32 || e.offset >= k32;
        std::string extra;
        if (big) {
            Put16(extra, 0x0001);
            Put16(extra, 24);
            Put64(extra, e.size);
            Put64(extra, e.size);
            Put64(extra, e.offset);
        }
        Put32(cd, kCentralSig);
        Put16(cd, 45);          // made by: MS-DOS / spec 4.5
        Put16(cd, big ? 45 : 20); // needed
        Put16(cd, kUtf8Flag);
        Put16(cd, 0);
        Put16(cd, e.dosTime);
        Put16(cd, e.dosDate);
        Put32(cd, e.crc);
        Put32(cd, big ? k32 : static_cast<uint32_t>(e.size));
        Put32(cd, big ? k32 : static_cast<uint32_t>(e.size));
        Put16(cd, static_cast<uint16_t>(e.name.size()));
        Put16(cd, static_cast<uint16_t>(extra.size()));
        Put16(cd, 0); // comment
        Put16(cd, 0); // disk
        Put16(cd, 0); // internal attrs
        Put32(cd, 0); // external attrs
        Put32(cd, big ? k32 : static_cast<uint32_t>(e.offset));
        cd += e.name;
        cd += extra;
    }
    if (!WriteAll(m_file, cd.data(), cd.size()))
        return Fail(error, "write failed (disk full?)");
    const uint64_t cdSize = cd.size();
    const uint64_t count = m_entries.size();
    std::string end;
    if (count >= 0xFFFF || cdSize >= k32 || cdOffset >= k32) {
        const uint64_t end64 = cdOffset + cdSize;
        Put32(end, kEnd64Sig);
        Put64(end, 44);
        Put16(end, 45);
        Put16(end, 45);
        Put32(end, 0);
        Put32(end, 0);
        Put64(end, count);
        Put64(end, count);
        Put64(end, cdSize);
        Put64(end, cdOffset);
        Put32(end, kLocator64Sig);
        Put32(end, 0);
        Put64(end, end64);
        Put32(end, 1);
    }
    Put32(end, kEndSig);
    Put16(end, 0);
    Put16(end, 0);
    Put16(end, static_cast<uint16_t>(std::min<uint64_t>(count, 0xFFFF)));
    Put16(end, static_cast<uint16_t>(std::min<uint64_t>(count, 0xFFFF)));
    Put32(end, static_cast<uint32_t>(std::min<uint64_t>(cdSize, k32)));
    Put32(end, static_cast<uint32_t>(std::min<uint64_t>(cdOffset, k32)));
    Put16(end, 0);
    const bool ok = WriteAll(m_file, end.data(), end.size());
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    if (!ok || !closed)
        return Fail(error, "write failed (disk full?)");
    return true;
}

// ---- Reader ------------------------------------------------------------------------

ZipReader::~ZipReader()
{
    if (m_file)
        std::fclose(m_file);
}

bool ZipReader::Open(const std::filesystem::path& file, std::string* error)
{
    if (_wfopen_s(&m_file, LongPath(file).c_str(), L"rb") != 0 || !m_file)
        return Fail(error, "can't open " + file.filename().string());
    _fseeki64(m_file, 0, SEEK_END);
    const int64_t fileSize = _ftelli64(m_file);
    const int64_t tailLen = std::min<int64_t>(fileSize, 65535 + 22 + 20);
    std::vector<uint8_t> tail(static_cast<size_t>(tailLen));
    _fseeki64(m_file, fileSize - tailLen, SEEK_SET);
    if (std::fread(tail.data(), 1, tail.size(), m_file) != tail.size())
        return Fail(error, "not a zip file");
    int64_t eocd = -1;
    for (int64_t i = tailLen - 22; i >= 0; --i)
        if (Get32(&tail[static_cast<size_t>(i)]) == kEndSig) {
            eocd = i;
            break;
        }
    if (eocd < 0)
        return Fail(error, "not a zip file");
    const uint8_t* e = &tail[static_cast<size_t>(eocd)];
    uint64_t count = Get16(e + 10), cdSize = Get32(e + 12), cdOffset = Get32(e + 16);
    if ((count == 0xFFFF || cdSize == k32 || cdOffset == k32) && eocd >= 20 && Get32(e - 20) == kLocator64Sig) {
        const uint64_t end64 = Get64(e - 20 + 8);
        std::array<uint8_t, 56> z{};
        _fseeki64(m_file, static_cast<int64_t>(end64), SEEK_SET);
        if (std::fread(z.data(), 1, z.size(), m_file) != z.size() || Get32(z.data()) != kEnd64Sig)
            return Fail(error, "damaged zip (zip64 end record)");
        count = Get64(&z[32]);
        cdSize = Get64(&z[40]);
        cdOffset = Get64(&z[48]);
    }
    if (cdOffset + cdSize > static_cast<uint64_t>(fileSize) || cdSize > (1ull << 31))
        return Fail(error, "damaged zip (central directory)");
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    _fseeki64(m_file, static_cast<int64_t>(cdOffset), SEEK_SET);
    if (std::fread(cd.data(), 1, cd.size(), m_file) != cd.size())
        return Fail(error, "damaged zip (central directory)");
    size_t pos = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (pos + 46 > cd.size() || Get32(&cd[pos]) != kCentralSig)
            return Fail(error, "damaged zip (entry header)");
        const uint8_t* h = &cd[pos];
        ZipEntry ze;
        ze.method = Get16(h + 10);
        ze.crc = Get32(h + 16);
        ze.compressedSize = Get32(h + 20);
        ze.size = Get32(h + 24);
        const uint16_t nameLen = Get16(h + 28), extraLen = Get16(h + 30), commentLen = Get16(h + 32);
        ze.localOffset = Get32(h + 42);
        if (pos + 46 + nameLen + extraLen + commentLen > cd.size())
            return Fail(error, "damaged zip (entry header)");
        ze.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);
        // ZIP64 extra: only the fields that were 0xFFFFFFFF, in this order.
        for (size_t x = 46 + nameLen; x + 4 <= 46u + nameLen + extraLen;) {
            const uint16_t id = Get16(h + x), len = Get16(h + x + 2);
            if (id == 0x0001) {
                size_t f = x + 4;
                if (ze.size == k32 && f + 8 <= x + 4 + len) {
                    ze.size = Get64(h + f);
                    f += 8;
                }
                if (ze.compressedSize == k32 && f + 8 <= x + 4 + len) {
                    ze.compressedSize = Get64(h + f);
                    f += 8;
                }
                if (ze.localOffset == k32 && f + 8 <= x + 4 + len)
                    ze.localOffset = Get64(h + f);
            }
            x += 4 + len;
        }
        m_entries.push_back(std::move(ze));
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipEntry* ZipReader::Find(std::string_view name) const
{
    for (const auto& e : m_entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

bool ZipReader::Read(const ZipEntry& e, const std::function<void(std::span<const uint8_t>)>& sink, std::stop_token st, std::string* error)
{
    if (!m_file)
        return Fail(error, "archive not open");
    if (e.method != 0)
        return Fail(error, e.name + " is compressed; only PhoneSweep's own (stored) zips can be read");
    std::array<uint8_t, 30> lh{};
    _fseeki64(m_file, static_cast<int64_t>(e.localOffset), SEEK_SET);
    if (std::fread(lh.data(), 1, lh.size(), m_file) != lh.size() || Get32(lh.data()) != kLocalSig)
        return Fail(error, "damaged zip (" + e.name + ")");
    _fseeki64(m_file, static_cast<int64_t>(e.localOffset + 30 + Get16(&lh[26]) + Get16(&lh[28])), SEEK_SET);
    std::vector<uint8_t> buf(kChunk);
    uint64_t left = e.size;
    uint32_t crc = 0;
    while (left > 0) {
        if (st.stop_requested())
            return Fail(error, "cancelled");
        const size_t n = std::fread(buf.data(), 1, static_cast<size_t>(std::min<uint64_t>(left, buf.size())), m_file);
        if (n == 0)
            return Fail(error, "damaged zip (" + e.name + " is truncated)");
        crc = Crc32({ buf.data(), n }, crc);
        sink({ buf.data(), n });
        left -= n;
    }
    if (crc != e.crc)
        return Fail(error, "damaged zip (" + e.name + " fails its CRC check)");
    return true;
}

std::optional<std::string> ZipReader::ReadAll(const ZipEntry& e, std::string* error)
{
    if (e.size > (256u << 20)) {
        Fail(error, "entry too large");
        return std::nullopt;
    }
    std::string out;
    out.reserve(static_cast<size_t>(e.size));
    if (!Read(e, [&](std::span<const uint8_t> d) { out.append(reinterpret_cast<const char*>(d.data()), d.size()); }, {}, error))
        return std::nullopt;
    return out;
}

bool ZipReader::Extract(const ZipEntry& e, const std::filesystem::path& to, std::stop_token st, std::string* error)
{
    std::FILE* out = nullptr;
    if (_wfopen_s(&out, LongPath(to).c_str(), L"wb") != 0 || !out)
        return Fail(error, "can't write " + to.filename().string());
    bool writeOk = true;
    const bool ok = Read(e, [&](std::span<const uint8_t> d) { writeOk = writeOk && WriteAll(out, d.data(), d.size()); }, st, error);
    const bool closed = std::fclose(out) == 0;
    if (ok && (!writeOk || !closed))
        return Fail(error, "write failed (disk full?)");
    return ok;
}

} // namespace ps
