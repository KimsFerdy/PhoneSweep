#include "scan/Categorize.h"

#include <algorithm>
#include <array>

namespace ps {

const char* CategoryName(Category c)
{
    switch (c) {
    case Category::Images: return "Images";
    case Category::Videos: return "Videos";
    case Category::Music: return "Music";
    case Category::Documents: return "Docs";
    case Category::Archives: return "APKs/Archives";
    case Category::Other: return "Other";
    default: return "?";
    }
}

const char* SourceName(Source s)
{
    switch (s) {
    case Source::Camera: return "Camera";
    case Source::Screenshots: return "Screenshots";
    case Source::WhatsApp: return "WhatsApp";
    case Source::WhatsAppBusiness: return "WhatsApp Business";
    case Source::Telegram: return "Telegram";
    case Source::Snapchat: return "Snapchat";
    case Source::Downloads: return "Downloads";
    case Source::Recordings: return "Recordings";
    case Source::Bluetooth: return "Bluetooth";
    case Source::Other: return "Other";
    default: return "?";
    }
}

const char* ProtectionName(Protection p)
{
    switch (p) {
    case Protection::AndroidData: return "Android/data";
    case Protection::AndroidObb: return "Android/obb";
    case Protection::Hidden: return "hidden folder";
    default: return "";
    }
}

namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool IEquals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

bool IStartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

bool IContains(std::string_view s, std::string_view needle)
{
    if (needle.size() > s.size())
        return false;
    for (size_t i = 0; i + needle.size() <= s.size(); ++i)
        if (IEquals(s.substr(i, needle.size()), needle))
            return true;
    return false;
}

struct ExtRule {
    std::string_view ext;
    Category cat;
};

constexpr std::array kExtRules = {
    // Images
    ExtRule{ "jpg", Category::Images }, ExtRule{ "jpeg", Category::Images }, ExtRule{ "png", Category::Images },
    ExtRule{ "gif", Category::Images }, ExtRule{ "webp", Category::Images }, ExtRule{ "heic", Category::Images },
    ExtRule{ "heif", Category::Images }, ExtRule{ "bmp", Category::Images }, ExtRule{ "dng", Category::Images },
    ExtRule{ "raw", Category::Images }, ExtRule{ "tif", Category::Images }, ExtRule{ "tiff", Category::Images },
    ExtRule{ "svg", Category::Images }, ExtRule{ "avif", Category::Images },
    // Videos
    ExtRule{ "mp4", Category::Videos }, ExtRule{ "mkv", Category::Videos }, ExtRule{ "mov", Category::Videos },
    ExtRule{ "avi", Category::Videos }, ExtRule{ "webm", Category::Videos }, ExtRule{ "3gp", Category::Videos },
    ExtRule{ "m4v", Category::Videos }, ExtRule{ "ts", Category::Videos }, ExtRule{ "flv", Category::Videos },
    ExtRule{ "wmv", Category::Videos }, ExtRule{ "mpg", Category::Videos }, ExtRule{ "mpeg", Category::Videos },
    // Music / audio
    ExtRule{ "mp3", Category::Music }, ExtRule{ "m4a", Category::Music }, ExtRule{ "aac", Category::Music },
    ExtRule{ "flac", Category::Music }, ExtRule{ "wav", Category::Music }, ExtRule{ "ogg", Category::Music },
    ExtRule{ "opus", Category::Music }, ExtRule{ "amr", Category::Music }, ExtRule{ "wma", Category::Music },
    ExtRule{ "mid", Category::Music }, ExtRule{ "midi", Category::Music }, ExtRule{ "3ga", Category::Music },
    // Documents
    ExtRule{ "pdf", Category::Documents }, ExtRule{ "doc", Category::Documents }, ExtRule{ "docx", Category::Documents },
    ExtRule{ "xls", Category::Documents }, ExtRule{ "xlsx", Category::Documents }, ExtRule{ "ppt", Category::Documents },
    ExtRule{ "pptx", Category::Documents }, ExtRule{ "txt", Category::Documents }, ExtRule{ "rtf", Category::Documents },
    ExtRule{ "odt", Category::Documents }, ExtRule{ "ods", Category::Documents }, ExtRule{ "odp", Category::Documents },
    ExtRule{ "csv", Category::Documents }, ExtRule{ "epub", Category::Documents }, ExtRule{ "md", Category::Documents },
    ExtRule{ "vcf", Category::Documents }, ExtRule{ "html", Category::Documents }, ExtRule{ "htm", Category::Documents },
    // APKs / archives
    ExtRule{ "apk", Category::Archives }, ExtRule{ "apks", Category::Archives }, ExtRule{ "xapk", Category::Archives },
    ExtRule{ "apkm", Category::Archives }, ExtRule{ "zip", Category::Archives }, ExtRule{ "rar", Category::Archives },
    ExtRule{ "7z", Category::Archives }, ExtRule{ "tar", Category::Archives }, ExtRule{ "gz", Category::Archives },
    ExtRule{ "tgz", Category::Archives }, ExtRule{ "bz2", Category::Archives }, ExtRule{ "xz", Category::Archives },
    ExtRule{ "obb", Category::Archives },
};

} // namespace

std::optional<Category> CategoryFromMime(std::string_view mime)
{
    if (mime.empty() || mime == "NULL")
        return std::nullopt;
    if (IStartsWith(mime, "image/"))
        return Category::Images;
    if (IStartsWith(mime, "video/"))
        return Category::Videos;
    if (IStartsWith(mime, "audio/"))
        return Category::Music;
    if (IEquals(mime, "application/vnd.android.package-archive") || IEquals(mime, "application/zip")
        || IEquals(mime, "application/x-rar-compressed") || IEquals(mime, "application/vnd.rar")
        || IEquals(mime, "application/x-7z-compressed") || IEquals(mime, "application/gzip")
        || IEquals(mime, "application/x-tar"))
        return Category::Archives;
    if (IEquals(mime, "application/pdf") || IStartsWith(mime, "text/") || IContains(mime, "officedocument")
        || IStartsWith(mime, "application/msword") || IStartsWith(mime, "application/vnd.ms-")
        || IStartsWith(mime, "application/vnd.oasis.opendocument") || IEquals(mime, "application/epub+zip"))
        return Category::Documents;
    return std::nullopt; // application/octet-stream and friends: fall back to the extension
}

Category CategoryFromName(std::string_view name)
{
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size())
        return Category::Other;
    const auto ext = name.substr(dot + 1);
    for (const auto& r : kExtRules)
        if (IEquals(ext, r.ext))
            return r.cat;
    return Category::Other;
}

Category Categorize(std::string_view fileName, std::string_view mime)
{
    if (auto c = CategoryFromMime(mime))
        return *c;
    return CategoryFromName(fileName);
}

Source SourceFromPath(std::string_view rel)
{
    // Messaging apps first: their folders also contain "Images"/"Video" subfolders.
    // Android 11+ moved them under Android/media/<package>/.
    if (IStartsWith(rel, "WhatsApp Business/") || IStartsWith(rel, "Android/media/com.whatsapp.w4b/"))
        return Source::WhatsAppBusiness;
    if (IStartsWith(rel, "WhatsApp/") || IStartsWith(rel, "Android/media/com.whatsapp/")
        || (IContains(rel.substr(0, rel.find('/')), "whatsapp") && rel.find('/') != std::string_view::npos))
        return Source::WhatsApp; // also mods like "TMWhatsApp/", "GBWhatsApp/"
    if (IStartsWith(rel, "Telegram/") || IStartsWith(rel, "Android/media/org.telegram.messenger/")
        || IStartsWith(rel, "Pictures/Telegram/") || IStartsWith(rel, "Movies/Telegram/")
        || IStartsWith(rel, "Download/Telegram/"))
        return Source::Telegram;
    if (IStartsWith(rel, "Snapchat/") || IStartsWith(rel, "Android/media/com.snapchat.android/"))
        return Source::Snapchat;
    if (IStartsWith(rel, "DCIM/Screenshots/") || IStartsWith(rel, "Pictures/Screenshots/") || IStartsWith(rel, "Screenshots/"))
        return Source::Screenshots;
    if (IStartsWith(rel, "DCIM/Camera/") || IStartsWith(rel, "DCIM/100ANDRO/") || IStartsWith(rel, "DCIM/OpenCamera/")
        || IStartsWith(rel, "GCam/") || IStartsWith(rel, "DCIM/GCam/"))
        return Source::Camera;
    if (IStartsWith(rel, "Download/Bluetooth/") || IStartsWith(rel, "Bluetooth/"))
        return Source::Bluetooth;
    if (IStartsWith(rel, "Download/") || IStartsWith(rel, "Downloads/"))
        return Source::Downloads;
    if (IStartsWith(rel, "Recordings/") || IStartsWith(rel, "Voice Recorder/") || IStartsWith(rel, "Call/")
        || IStartsWith(rel, "Sounds/") || IStartsWith(rel, "Music/Recordings/"))
        return Source::Recordings;
    return Source::Other;
}

Protection ProtectionFromPath(std::string_view rel)
{
    if (IStartsWith(rel, "Android/data/") || IEquals(rel, "Android/data"))
        return Protection::AndroidData;
    if (IStartsWith(rel, "Android/obb/") || IEquals(rel, "Android/obb"))
        return Protection::AndroidObb;
    // Any dot-folder along the path (not the file name itself: ".nomedia" in DCIM is fine).
    const auto lastSlash = rel.rfind('/');
    if (lastSlash == std::string_view::npos)
        return Protection::None;
    const auto dirs = rel.substr(0, lastSlash);
    size_t start = 0;
    while (start <= dirs.size()) {
        const auto end = dirs.find('/', start);
        const auto comp = dirs.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!comp.empty() && comp.front() == '.')
            return Protection::Hidden;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return Protection::None;
}

void Classify(FileEntry& e, std::string_view volumeRoot)
{
    std::string_view p = e.path;
    e.relOffset = p.starts_with(volumeRoot) ? static_cast<uint32_t>(volumeRoot.size()) : 0u;
    const auto slash = p.rfind('/');
    e.nameOffset = slash == std::string_view::npos ? 0u : static_cast<uint32_t>(slash + 1);
    if (e.nameOffset < e.relOffset)
        e.nameOffset = e.relOffset;
    const auto rel = e.RelPath();
    e.category = Categorize(e.Name(), e.mime);
    e.source = SourceFromPath(rel);
    e.protection = ProtectionFromPath(rel);
}

} // namespace ps
