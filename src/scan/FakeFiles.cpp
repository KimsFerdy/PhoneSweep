#include "scan/FakeFiles.h"

#include "scan/Categorize.h"

#include <array>
#include <format>
#include <random>

namespace ps {

std::vector<Volume> FakeVolumes()
{
    return {
        { "/storage/emulated/0/", "Internal", false, 128'000'000'000ull, 121'200'000'000ull, 6'800'000'000ull },
        { "/storage/3A1F-0C22/", "SD card", true, 64'000'000'000ull, 41'000'000'000ull, 23'000'000'000ull },
    };
}

std::vector<FileEntry> FakeFiles(size_t count)
{
    struct Pattern {
        const char* folder;
        const char* nameFmt; // {} = number
        uint64_t minSize, maxSize;
        int weight;
        int volume;
    };
    static constexpr std::array kPatterns = {
        Pattern{ "DCIM/Camera", "IMG_2026{:04}.jpg", 1'500'000, 6'000'000, 30, 0 },
        Pattern{ "DCIM/Camera", "VID_2026{:04}.mp4", 20'000'000, 3'100'000'000, 4, 0 },
        Pattern{ "DCIM/Screenshots", "Screenshot_2026{:04}_One UI.png", 300'000, 2'500'000, 8, 0 },
        Pattern{ "Android/media/com.whatsapp/WhatsApp/Media/WhatsApp Images", "IMG-2026{:04}-WA0001.jpg", 80'000, 400'000, 30, 0 },
        Pattern{ "Android/media/com.whatsapp/WhatsApp/Media/WhatsApp Video", "VID-2026{:04}-WA0003.mp4", 2'000'000, 90'000'000, 8, 0 },
        Pattern{ "Android/media/com.whatsapp/WhatsApp/Media/WhatsApp Voice Notes/202644", "PTT-2026{:04}-WA0007.opus", 10'000, 600'000, 10, 0 },
        Pattern{ "Telegram/Telegram Video", "{}_Kinshasa ya kala – ndako.mp4", 5'000'000, 400'000'000, 3, 0 },
        Pattern{ "Download", "Facture_Électricité_{}.pdf", 50'000, 3'000'000, 4, 0 },
        Pattern{ "Download", "app-release-{}.apk", 8'000'000, 120'000'000, 2, 0 },
        Pattern{ "Music", "Fally Ipupa – Éloko Oyo ({}).mp3", 3'000'000, 12'000'000, 4, 0 },
        Pattern{ "Documents", "Ripoti ya mwezi {}.docx", 20'000, 900'000, 2, 0 },
        Pattern{ "Android/data/com.spotify.music/files/spotifycache/Storage", "{:08x}.file", 100'000, 4'000'000, 5, 0 },
        Pattern{ ".thumbnails", "{}.jpg", 5'000, 60'000, 4, 0 },
        Pattern{ "DCIM/Camera", "20250{:04}_181203.jpg", 2'000'000, 7'000'000, 10, 1 },
        Pattern{ "Movies", "Mbóka ya biso – épisode {}.mkv", 300'000'000, 1'800'000'000, 1, 1 },
    };

    int totalWeight = 0;
    for (const auto& p : kPatterns)
        totalWeight += p.weight;

    const auto vols = FakeVolumes();
    std::mt19937_64 rng(42);
    std::vector<FileEntry> out;
    out.reserve(count);
    const int64_t now = 1'794'000'000; // fixed so screenshots are stable
    for (size_t i = 0; i < count; ++i) {
        int pick = static_cast<int>(rng() % static_cast<uint64_t>(totalWeight));
        const Pattern* pat = &kPatterns[0];
        for (const auto& p : kPatterns) {
            if (pick < p.weight) {
                pat = &p;
                break;
            }
            pick -= p.weight;
        }
        FileEntry e;
        const auto& vol = vols[static_cast<size_t>(pat->volume)];
        e.path = std::format("{}{}/", vol.root, pat->folder) + std::vformat(pat->nameFmt, std::make_format_args(i));
        // Skew sizes toward the small end like real phones.
        const double u = static_cast<double>(rng() % 10000) / 10000.0;
        e.size = pat->minSize + static_cast<uint64_t>(static_cast<double>(pat->maxSize - pat->minSize) * u * u * u);
        e.mtime = now - static_cast<int64_t>(rng() % (3ull * 365 * 86400));
        e.volume = static_cast<uint8_t>(pat->volume);
        Classify(e, vol.root);
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace ps
