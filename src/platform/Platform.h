#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

struct ImVec4;

namespace ps {

struct PlatformConfig {
    std::wstring title = L"PhoneSweep";
    int width = 1280; // unscaled, multiplied by monitor DPI
    int height = 800;
    bool visible = true;
};

// Window + renderer + ImGui backend glue. Swap this out for GLFW/OpenGL later
// without touching any screen code.
class IPlatform {
public:
    virtual ~IPlatform() = default;

    // Creates the window, device and ImGui backends. ImGui context must exist.
    virtual bool Init(const PlatformConfig& cfg) = 0;
    virtual void Shutdown() = 0;

    // Pumps OS messages. Returns false when the app should quit.
    virtual bool PumpEvents() = 0;
    // True while minimised/occluded: skip rendering and sleep.
    virtual bool IsIdle() const = 0;

    virtual void NewFrame() = 0;
    virtual void Render(const ImVec4& clearColor) = 0;
    virtual void Present() = 0;

    // Monitor DPI scale for the window (1.0 = 96 dpi). Changes when dragged across monitors.
    virtual float DpiScale() const = 0;
    // True once after the DPI changed; caller rebuilds style.
    virtual bool ConsumeDpiChanged() = 0;

    // Writes the last rendered frame to a PNG. Call after Render(), before Present().
    virtual bool SaveScreenshot(const std::filesystem::path& png) = 0;

    // RGBA8 texture for ImGui::Image / AddImage. Returns 0 on failure.
    virtual uint64_t CreateTexture(const uint8_t* rgba, int width, int height) = 0;
    virtual void DestroyTexture(uint64_t texture) = 0;

    // Native dialogs.
    virtual std::optional<std::filesystem::path> OpenFileDialog(const wchar_t* filterName, const wchar_t* pattern) = 0;
    virtual std::optional<std::filesystem::path> PickFolder(const std::filesystem::path& initial) = 0;
    virtual void SetClipboardText(const std::string& utf8) = 0;
    virtual void OpenInExplorer(const std::filesystem::path& path) = 0;
    virtual void OpenUrl(const std::string& utf8Url) = 0; // default browser
};

std::unique_ptr<IPlatform> CreateWin32Dx11Platform();

} // namespace ps
