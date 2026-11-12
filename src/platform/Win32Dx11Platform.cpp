#include "platform/Platform.h"

#include "util/Log.h"
#include "util/Utf8.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <stb_image_write.h>

#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ps {

namespace {

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

class Win32Dx11Platform final : public IPlatform {
public:
    bool Init(const PlatformConfig& cfg) override;
    void Shutdown() override;
    bool PumpEvents() override;
    bool IsIdle() const override { return m_minimized || m_occluded; }
    void NewFrame() override;
    void Render(const ImVec4& clearColor) override;
    void Present() override;
    float DpiScale() const override { return m_dpi; }
    bool ConsumeDpiChanged() override { return std::exchange(m_dpiChanged, false); }
    bool SaveScreenshot(const std::filesystem::path& png) override;
    uint64_t CreateTexture(const uint8_t* rgba, int width, int height) override;
    void DestroyTexture(uint64_t texture) override;
    std::optional<std::filesystem::path> OpenFileDialog(const wchar_t* filterName, const wchar_t* pattern) override;
    std::optional<std::filesystem::path> PickFolder(const std::filesystem::path& initial) override;
    void SetClipboardText(const std::string& utf8) override;
    void OpenInExplorer(const std::filesystem::path& path) override;
    void OpenUrl(const std::string& utf8Url) override;

private:
    static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    bool CreateDevice();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    HWND m_hwnd = nullptr;
    WNDCLASSEXW m_wc{};
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    UINT m_resizeW = 0, m_resizeH = 0;
    bool m_minimized = false;
    bool m_occluded = false;
    float m_dpi = 1.0f;
    bool m_dpiChanged = false;
    bool m_quit = false;
};

bool Win32Dx11Platform::Init(const PlatformConfig& cfg)
{
    ImGui_ImplWin32_EnableDpiAwareness();

    m_wc = { sizeof(m_wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW),
        nullptr, nullptr, L"PhoneSweepWindow", nullptr };
    // App icon from the .rc resource (id 1), at the sizes Windows asks for.
    m_wc.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    m_wc.hIconSm = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&m_wc);

    // Size the window for the DPI of the monitor it will open on.
    POINT origin{ 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    m_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    const int w = std::min(static_cast<int>(cfg.width * m_dpi), workW);
    const int h = std::min(static_cast<int>(cfg.height * m_dpi), workH);
    const int x = mi.rcWork.left + (workW - w) / 2, y = mi.rcWork.top + (workH - h) / 2;

    m_hwnd = CreateWindowExW(0, m_wc.lpszClassName, cfg.title.c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
        m_wc.hInstance, this);
    if (!m_hwnd) {
        LogError("CreateWindowExW failed (error {})", GetLastError());
        return false;
    }

    // Dark title bar to match the theme (Windows 10 20H1+ / 11).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = RGB(0x0F, 0x11, 0x15);
    DwmSetWindowAttribute(m_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));

    if (!CreateDevice()) {
        LogError("Failed to create a Direct3D 11 device");
        return false;
    }

    // Re-read DPI from the actual window (it may have landed on another monitor).
    m_dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd);

    ShowWindow(m_hwnd, cfg.visible ? SW_SHOWDEFAULT : SW_HIDE);
    UpdateWindow(m_hwnd);

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device, m_context);
    return true;
}

void Win32Dx11Platform::Shutdown()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    CleanupRenderTarget();
    SafeRelease(m_swapChain);
    SafeRelease(m_context);
    SafeRelease(m_device);
    if (m_hwnd)
        DestroyWindow(m_hwnd);
    UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
}

bool Win32Dx11Platform::CreateDevice()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &sd, &m_swapChain, &m_device, &got, &m_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) // no GPU (RDP, VMs): fall back to WARP
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
            &m_swapChain, &m_device, &got, &m_context);
    if (FAILED(hr))
        return false;
    CreateRenderTarget();
    return true;
}

void Win32Dx11Platform::CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        m_device->CreateRenderTargetView(back, nullptr, &m_rtv);
        back->Release();
    }
}

void Win32Dx11Platform::CleanupRenderTarget() { SafeRelease(m_rtv); }

bool Win32Dx11Platform::PumpEvents()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT)
            m_quit = true;
    }
    if (m_quit)
        return false;

    if (m_occluded && m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        return true;
    m_occluded = false;

    if (m_resizeW != 0 && m_resizeH != 0) {
        CleanupRenderTarget();
        m_swapChain->ResizeBuffers(0, m_resizeW, m_resizeH, DXGI_FORMAT_UNKNOWN, 0);
        m_resizeW = m_resizeH = 0;
        CreateRenderTarget();
    }
    return true;
}

void Win32Dx11Platform::NewFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
}

void Win32Dx11Platform::Render(const ImVec4& c)
{
    const float clear[4] = { c.x, c.y, c.z, c.w };
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_context->ClearRenderTargetView(m_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Win32Dx11Platform::Present()
{
    const HRESULT hr = m_swapChain->Present(1, 0); // vsync
    m_occluded = (hr == DXGI_STATUS_OCCLUDED);
}

bool Win32Dx11Platform::SaveScreenshot(const std::filesystem::path& png)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &staging))) {
        m_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::vector<unsigned char> rgba(static_cast<size_t>(desc.Width) * desc.Height * 4);
            for (UINT y = 0; y < desc.Height; ++y) {
                memcpy(&rgba[static_cast<size_t>(y) * desc.Width * 4], static_cast<unsigned char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
                for (UINT x = 0; x < desc.Width; ++x)
                    rgba[(static_cast<size_t>(y) * desc.Width + x) * 4 + 3] = 255;
            }
            m_context->Unmap(staging, 0);
            ok = stbi_write_png(Narrow(png.wstring()).c_str(), static_cast<int>(desc.Width), static_cast<int>(desc.Height), 4,
                     rgba.data(), static_cast<int>(desc.Width) * 4)
                != 0;
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

uint64_t Win32Dx11Platform::CreateTexture(const uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{ rgba, static_cast<UINT>(width) * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_device->CreateTexture2D(&desc, &init, &tex)))
        return 0;
    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = m_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release(); // the SRV keeps the texture alive
    if (FAILED(hr))
        return 0;
    // The DX11 backend's ImTextureID is the SRV pointer.
    return reinterpret_cast<uint64_t>(srv);
}

void Win32Dx11Platform::DestroyTexture(uint64_t texture)
{
    if (texture)
        reinterpret_cast<ID3D11ShaderResourceView*>(texture)->Release();
}

std::optional<std::filesystem::path> Win32Dx11Platform::OpenFileDialog(const wchar_t* filterName, const wchar_t* pattern)
{
    std::wstring filter = std::wstring(filterName) + L'\0' + pattern + L'\0' + L"All files" + L'\0' + L"*.*" + L'\0' + L'\0';
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return std::nullopt;
    return std::filesystem::path(file);
}

std::optional<std::filesystem::path> Win32Dx11Platform::PickFolder(const std::filesystem::path& initial)
{
    std::optional<std::filesystem::path> result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        std::error_code ec;
        if (!initial.empty() && std::filesystem::exists(initial, ec)) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                dlg->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(m_hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return result;
}

void Win32Dx11Platform::SetClipboardText(const std::string& utf8) { ImGui::SetClipboardText(utf8.c_str()); }

void Win32Dx11Platform::OpenInExplorer(const std::filesystem::path& path)
{
    ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Win32Dx11Platform::OpenUrl(const std::string& utf8Url)
{
    // Only web links: never hand an arbitrary string to the shell.
    if (!utf8Url.starts_with("https://"))
        return;
    ShellExecuteW(m_hwnd, L"open", Widen(utf8Url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT WINAPI Win32Dx11Platform::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Win32Dx11Platform*>(cs->lpCreateParams);
        // CreateWindowExW hasn't returned yet, so m_hwnd is still null here.
        self->m_hwnd = hWnd;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (auto* self = reinterpret_cast<Win32Dx11Platform*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA)))
        return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT Win32Dx11Platform::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        m_minimized = (wParam == SIZE_MINIMIZED);
        if (!m_minimized) {
            m_resizeW = LOWORD(lParam);
            m_resizeH = HIWORD(lParam);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = { static_cast<LONG>(860 * m_dpi), static_cast<LONG>(600 * m_dpi) };
        return 0;
    }
    case WM_DPICHANGED: {
        m_dpi = HIWORD(wParam) / 96.0f;
        m_dpiChanged = true;
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // no ALT menu beep
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

} // namespace

std::unique_ptr<IPlatform> CreateWin32Dx11Platform() { return std::make_unique<Win32Dx11Platform>(); }

} // namespace ps
