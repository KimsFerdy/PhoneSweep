#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ps {

enum class Category : uint8_t { Images, Videos, Music, Documents, Archives, Other, Count };
inline constexpr size_t kCategoryCount = static_cast<size_t>(Category::Count);

// Where a file came from, inferred from its path. Messaging media matters most.
enum class Source : uint8_t {
    Camera,
    Screenshots,
    WhatsApp,
    WhatsAppBusiness,
    Telegram,
    Snapchat,
    Downloads,
    Recordings,
    Bluetooth,
    Other,
    Count
};
inline constexpr size_t kSourceCount = static_cast<size_t>(Source::Count);

// Why a path is off-limits by default (shown greyed; never nuked unless unlocked).
enum class Protection : uint8_t { None, AndroidData, AndroidObb, Hidden };

enum FileFlags : uint8_t {
    FileFlag_None = 0,
    FileFlag_Keep = 1 << 0, // user marked "keep" (to be saved before nuke)
};

// A storage volume on the phone: internal shared storage or an SD card.
struct Volume {
    std::string root;  // "/storage/emulated/0/" (always with trailing slash)
    std::string label; // "Internal" / "SD card"
    bool removable = false;
    uint64_t totalBytes = 0, usedBytes = 0, freeBytes = 0; // from df, 0 if unknown
};

struct FileEntry {
    std::string path;         // absolute device path, raw bytes (usually UTF-8)
    uint64_t size = 0;        // apparent size (what a copy transfers)
    uint64_t diskBytes = 0;   // allocated on the phone; 0 = unknown
    int64_t mtime = 0;        // unix seconds
    uint32_t relOffset = 0;   // path.substr(relOffset) = path relative to the volume root
    uint32_t nameOffset = 0;  // path.substr(nameOffset) = file name
    uint8_t volume = 0;       // index into the scan's volume list
    Category category = Category::Other;
    Source source = Source::Other;
    Protection protection = Protection::None;
    uint8_t flags = FileFlag_None;
    std::string mime;         // from MediaStore when available

    std::string_view Name() const { return std::string_view(path).substr(nameOffset); }
    std::string_view RelPath() const { return std::string_view(path).substr(relOffset); }
    // Folder relative to the volume root, without trailing slash ("" for root files).
    std::string_view RelFolder() const
    {
        if (nameOffset <= relOffset)
            return {};
        return std::string_view(path).substr(relOffset, nameOffset - relOffset - 1);
    }
    bool IsProtected() const { return protection != Protection::None; }
    // Space it takes on the phone (= what deleting it frees). Sparse files are much
    // smaller on disk than their size; small files round up to a block.
    uint64_t Disk() const { return diskBytes ? diskBytes : size; }
};

const char* CategoryName(Category c);
const char* SourceName(Source s);
const char* ProtectionName(Protection p);

} // namespace ps
