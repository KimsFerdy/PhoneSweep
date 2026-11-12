#include "cleanup/Preview.h"

#include "adb/AdbClient.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace ps {

namespace {

std::string LowerExt(std::string_view name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size())
        return {};
    std::string ext(name.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string FromUtf16(std::string_view b, bool bigEndian)
{
    std::string out;
    out.reserve(b.size());
    auto unit = [&](size_t i) -> uint32_t {
        const auto lo = static_cast<uint8_t>(b[i + (bigEndian ? 1 : 0)]);
        const auto hi = static_cast<uint8_t>(b[i + (bigEndian ? 0 : 1)]);
        return static_cast<uint32_t>(hi) << 8 | lo;
    };
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        uint32_t cp = unit(i);
        if (cp >= 0xD800 && cp < 0xDC00) {
            if (i + 3 >= b.size())
                break; // cut surrogate pair at the end
            const uint32_t lo = unit(i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// Length of a valid UTF-8 sequence starting at s[i], 0 if invalid, -1 if cut by the end.
int Utf8Len(std::string_view s, size_t i)
{
    const auto c = static_cast<uint8_t>(s[i]);
    int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n <= 1)
        return n;
    for (int k = 1; k < n; ++k) {
        if (i + k >= s.size())
            return -1;
        if ((static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80)
            return 0;
    }
    return n;
}

} // namespace

bool IsTextName(std::string_view fileName)
{
    static constexpr std::array<std::string_view, 34> kExt = { "txt", "log", "md", "markdown", "json", "csv", "tsv", "xml", "html", "htm",
        "ini", "cfg", "conf", "yaml", "yml", "toml", "srt", "vtt", "ass", "lrc", "nfo", "properties", "sql", "css", "js", "ts", "py", "java",
        "kt", "c", "cpp", "h", "sh", "vcf" };
    const std::string ext = LowerExt(fileName);
    return !ext.empty() && std::ranges::find(kExt, ext) != kExt.end();
}

PreviewKind PreviewKindFor(const FileEntry& e)
{
    switch (e.category) {
    case Category::Images: return PreviewKind::Image;
    case Category::Videos: return PreviewKind::Video;
    case Category::Music: return PreviewKind::Audio;
    default: break;
    }
    return IsTextName(e.Name()) ? PreviewKind::Text : PreviewKind::Other;
}

std::optional<std::string> DecodeTextPreview(std::string_view bytes, size_t maxLines)
{
    std::string text;
    if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xFE) {
        text = FromUtf16(bytes.substr(2), false);
    } else if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFE && static_cast<uint8_t>(bytes[1]) == 0xFF) {
        text = FromUtf16(bytes.substr(2), true);
    } else {
        if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF")
            bytes.remove_prefix(3);
        // Binary check on the raw bytes: any NUL, or more than 2% odd control characters.
        size_t control = 0;
        for (const char ch : bytes) {
            const auto c = static_cast<uint8_t>(ch);
            if (c == 0)
                return std::nullopt;
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 0x1B)
                ++control;
        }
        if (control * 50 > bytes.size())
            return std::nullopt;
        // Valid UTF-8 is copied as is; stray bytes are taken as Latin-1.
        text.reserve(bytes.size());
        for (size_t i = 0; i < bytes.size();) {
            const int n = Utf8Len(bytes, i);
            if (n < 0)
                break; // character cut by the 64 KB limit
            if (n == 0) {
                AppendUtf8(text, static_cast<uint8_t>(bytes[i]));
                ++i;
            } else {
                text.append(bytes.substr(i, static_cast<size_t>(n)));
                i += static_cast<size_t>(n);
            }
        }
    }

    // Normalise line ends and tabs, drop other control characters, cap the line count.
    std::string out;
    out.reserve(text.size());
    size_t lines = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                continue;
            out += '\n';
        } else if (c == '\t') {
            out += "    ";
        } else if (static_cast<uint8_t>(c) < 0x20 && c != '\n') {
            continue;
        } else {
            out += c;
        }
        if (!out.empty() && out.back() == '\n' && ++lines >= maxLines)
            break;
    }
    return out;
}

std::optional<std::string> FetchFileHead(const AdbClient& adb, const std::string& serial, const std::string& path, size_t bytes,
    std::stop_token st)
{
    const auto r = adb.ExecOut(serial, ShellCommand("head").arg("-c").arg(std::to_string(bytes)).arg(path), { .timeout = 30s, .cancel = st });
    if (!r.ok())
        return std::nullopt;
    return r.out;
}

} // namespace ps
