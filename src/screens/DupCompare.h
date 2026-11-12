#pragma once
#include "cleanup/Cleanup.h"
#include "platform/AudioPlayer.h"
#include "util/JobSystem.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps {

struct AppContext;
struct FileEntry;

// v1.1: one duplicate group, every copy side by side (image / video frame / audio
// player / text), each with its own Keep / Delete choice. Selection only: the
// actual delete still goes through the list's Delete button and its confirmation.
class DupCompare {
public:
    using Lookup = std::function<const FileEntry*(std::string_view path)>;

    DupCompare();
    ~DupCompare();

    void Open(int group);
    bool IsOpen() const { return m_group >= 0; }
    void Close();
    void Draw(AppContext& ctx, std::vector<DupGroup>& groups, std::vector<bool>& ticked, const Lookup& lookup);

private:
    struct TextFetch {
        std::mutex mutex;
        bool done = false;
        std::optional<std::string> text; // nullopt = binary or unreadable
        bool unreadable = false;
    };
    enum class After : uint8_t { Play, OpenOnPc };
    struct Pull {
        std::mutex mutex;
        bool done = false, ok = false;
        uint64_t bytes = 0, total = 0;
        std::filesystem::path local;
        std::string error;
        After after = After::OpenOnPc;
        bool handled = false;
        bool converting = false; // ffmpeg is making a playable WAV
        JobPtr job;
    };

    void DrawColumn(AppContext& ctx, DupGroup& grp, bool& ticked, size_t i, const FileEntry* e, float width, float height);
    void DrawPreview(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b);
    void DrawMediaButtons(AppContext& ctx, const FileEntry& e, float width);
    TextFetch& Text(AppContext& ctx, const FileEntry& e);
    // Local copy for playing / opening: the verified backup if there is one, else pulled to %TEMP%.
    void RequestLocal(AppContext& ctx, const FileEntry& e, After after);
    void PumpPulls(AppContext& ctx);
    void ReleaseFiles();

    int m_group = -1;
    bool m_justOpened = false;
    std::unordered_map<std::string, std::shared_ptr<TextFetch>> m_text;
    std::unordered_map<std::string, std::shared_ptr<Pull>> m_pulls;
    AudioPlayer m_audio;
    std::filesystem::path m_tempDir;
};

} // namespace ps
