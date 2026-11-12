#pragma once
#include "platform/AudioPlayer.h"
#include "util/JobSystem.h"

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ps {

struct AppContext;
struct FileEntry;

// Shared by the duplicate compare window and the file preview window: renders one
// file (image / video frame / audio player / text) and handles "Open on PC"
// (saved backup, or a copy pulled to a private temp folder). UI thread only.
class PreviewKit {
public:
    PreviewKit();
    ~PreviewKit();
    PreviewKit(const PreviewKit&) = delete;
    PreviewKit& operator=(const PreviewKit&) = delete;

    // Once per frame while the owning window is open: finished copies, player errors.
    void Pump(AppContext& ctx);
    // Preview in [a, b]; media buttons (Play / Open on PC) take the bottom row.
    // zoomable: photos use the 2048 px variant; wheel zooms, drag pans, double-click resets.
    void Draw(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b, bool zoomable = false);
    // Opens the file in its default Windows app (media only, see CanOpenOnPc).
    void OpenOnPc(AppContext& ctx, const FileEntry& e);
    static bool CanOpenOnPc(const FileEntry& e);

    void StopAudio() { m_audio.Close(); }
    void ResetZoom() { m_zoom = 1.0f; m_pan = ImVec2(0, 0); }
    // Stops playback, cancels copies and deletes this kit's temp files.
    void Release();

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

    void DrawPreview(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b, bool zoomable);
    void DrawZoomable(AppContext& ctx, const FileEntry& e, ImVec2 a, ImVec2 b);
    void DrawMediaButtons(AppContext& ctx, const FileEntry& e, float width);
    TextFetch& Text(AppContext& ctx, const FileEntry& e);
    void RequestLocal(AppContext& ctx, const FileEntry& e, After after);
    void PumpPulls(AppContext& ctx);

    std::unordered_map<std::string, std::shared_ptr<TextFetch>> m_text;
    std::unordered_map<std::string, std::shared_ptr<Pull>> m_pulls;
    AudioPlayer m_audio;
    std::filesystem::path m_tempDir;
    float m_zoom = 1.0f;
    ImVec2 m_pan{ 0, 0 };
    std::string m_zoomKey; // file the zoom belongs to
};

// "Internal · DCIM/Camera", "SD card · Music": for captions.
std::string FolderCaption(std::string_view devicePath);

} // namespace ps
