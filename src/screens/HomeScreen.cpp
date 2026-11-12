#include "screens/HomeScreen.h"

#include "app/AppContext.h"
#include "platform/Platform.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <vector>

namespace ps {

using namespace ps::theme;

namespace {

constexpr float kHeaderH = 52.0f;
constexpr float kFooterH = 40.0f;
constexpr const char* kPlatformToolsUrl = "https://developer.android.com/tools/releases/platform-tools";

void VSpace(float px) { ImGui::Dummy(ImVec2(0, S(px))); }

} // namespace

void HomeScreen::Draw(AppContext& ctx)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), 0));
    ImGui::Begin("##Home", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
    ImGui::PopStyleVar(3);

    DrawHeader(ctx);

    const float centerH = ImGui::GetContentRegionAvail().y - S(kFooterH);
    ImGui::BeginChild("##center", ImVec2(0, centerH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    {
        const auto& snap = ctx.devices;
        int ready = 0, unauthorized = 0;
        for (const auto& e : snap.entries) {
            ready += e.state == DeviceEntry::State::Device;
            unauthorized += e.state == DeviceEntry::State::Unauthorized;
        }

        const float target = ready == 1 ? 1.0f : 0.0f;
        const float dt = ImGui::GetIO().DeltaTime;
        m_readyAnim += (target - m_readyAnim) * std::min(1.0f, dt * 9.0f);
        if (std::abs(target - m_readyAnim) < 0.002f)
            m_readyAnim = target;

        if (!snap.adbFound)
            DrawNoAdb(ctx);
        else if (ready > 0)
            DrawReadyDevices(ctx);
        else
            DrawWaiting(ctx, unauthorized > 0);
        ImGui::Dummy(ImVec2(0, 0)); // close out any trailing SetCursorPos
    }
    ImGui::EndChild();

    DrawFooter(ctx);
    ImGui::End();
}

void HomeScreen::DrawHeader(AppContext& ctx)
{
    const float h = S(kHeaderH);
    const float y0 = ImGui::GetCursorPosY();
    {
        Font f(GetFonts().uiStrong, 17.0f);
        const float logo = S(30);
        ImGui::SetCursorPosY(y0 + (h - logo) * 0.5f);
        ui::Logo(logo);
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(y0 + (h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted("PhoneSweep");
    }
    const char* wifi = ICON_WIFI "  Wi-Fi";
    const char* label = ICON_GEAR "  Settings";
    const float pad = ImGui::GetStyle().FramePadding.x * 2;
    const float bw = ui::TextWidth(label) + pad;
    const float ww = ui::TextWidth(wifi) + pad;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw - ww - S(8));
    ImGui::SetCursorPosY(y0 + (h - ImGui::GetFrameHeight()) * 0.5f);
    if (ui::GhostButton(wifi))
        ctx.showWireless = true;
    ImGui::SetItemTooltip("Connect a phone over Wi-Fi (no cable)");
    ImGui::SameLine(0, S(8));
    ImGui::SetCursorPosY(y0 + (h - ImGui::GetFrameHeight()) * 0.5f);
    if (ui::GhostButton(label))
        ctx.showSettings = true;
    ImGui::SetCursorPosY(y0 + h);
}

void HomeScreen::DrawFooter(AppContext& ctx)
{
    const auto& snap = ctx.devices;
    const float y0 = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y0 + (S(kFooterH) - ImGui::GetTextLineHeight()) * 0.5f - S(4));
    Font f(GetFonts().ui, FontSmall);

    if (snap.adbFound) {
        ui::ColoredText(Success, "adb " ICON_CHECK);
        ImGui::SameLine(0, S(6));
        ui::DimText(snap.adbVersion.empty() ? std::string("found") : std::format("found (v{})", snap.adbVersion));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", snap.adbPath.c_str());
    } else {
        ui::ColoredText(Danger, "adb " ICON_XMARK);
        ImGui::SameLine(0, S(6));
        ui::DimText("not found");
    }

    int ready = 0, unauth = 0;
    for (const auto& e : snap.entries) {
        ready += e.state == DeviceEntry::State::Device;
        unauth += e.state == DeviceEntry::State::Unauthorized;
    }
    std::string status;
    bool spin = false;
    if (!snap.adbFound) {
        status = "adb required";
    } else if (!snap.lastError.empty()) {
        status = "adb error, retrying\xE2\x80\xA6";
        spin = true;
    } else if (ready == 1) {
        status = "1 phone ready";
    } else if (ready > 1) {
        status = std::format("{} phones ready", ready);
    } else if (unauth > 0) {
        status = "Waiting for authorization\xE2\x80\xA6";
        spin = true;
    } else {
        status = "Waiting for device\xE2\x80\xA6";
        spin = true;
    }

    const float spinW = spin ? S(14) + S(8) : 0.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ui::TextWidth(status) - spinW);
    ui::DimText(status);
    if (spin) {
        ImGui::SameLine(0, S(8));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - S(14)) * 0.5f);
        ui::Spinner(S(7), S(2), TextDim);
    }
}

void HomeScreen::DrawNoAdb(AppContext& ctx)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cardW = std::min(S(560), avail.x);
    ImGui::SetCursorPosY(std::max(0.0f, (avail.y - m_lastContentHeight) * 0.5f));
    const float startY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX((avail.x - cardW) * 0.5f);

    ui::BeginCard("##noadb", cardW);
    {
        Font f(GetFonts().ui, 28.0f);
        ui::ColoredText(Warning, ICON_WARNING);
    }
    VSpace(2);
    {
        Font f(GetFonts().uiStrong, FontH2);
        ImGui::TextUnformatted("adb.exe not found");
    }
    ui::DimText("PhoneSweep talks to your phone through Google's Android platform-tools. "
                "It looks next to PhoneSweep.exe, then in a platform-tools folder, then on your PATH.");
    VSpace(6);
    ImGui::TextUnformatted("1.  Download \"SDK Platform-Tools for Windows\" from:");
    {
        Font f(GetFonts().mono, FontSmall);
        ImGui::Indent(S(26));
        ui::ColoredText(Info, kPlatformToolsUrl);
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(2)));
        if (ui::GhostButton(ICON_COPY "##copyurl")) {
            ctx.platform.SetClipboardText(kPlatformToolsUrl);
            ui::Toast("Copied to clipboard");
        }
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("Copy link");
        ImGui::Unindent(S(26));
    }
    ImGui::TextUnformatted("2.  Unzip it so platform-tools sits next to PhoneSweep.exe.");
    ImGui::TextUnformatted("3.  Click Retry, or point PhoneSweep at adb.exe yourself.");
    VSpace(10);
    if (ui::PrimaryButton(ICON_ROTATE "  Retry"))
        ctx.requestAdbRetry = true;
    ImGui::SameLine();
    if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Locate adb.exe\xE2\x80\xA6"))
        ctx.requestAdbRelocate = true;
    ui::EndCard();

    m_lastContentHeight = ImGui::GetCursorPosY() - startY;
}

void HomeScreen::DrawWaiting(AppContext& ctx, bool unauthorized)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const double t = ImGui::GetTime();

    ImGui::SetCursorPosY(std::max(S(8), (avail.y - m_lastContentHeight) * 0.5f));
    const float startY = ImGui::GetCursorPosY();

    const float phoneH = std::clamp(avail.y * 0.36f, S(150), S(230));
    const float areaH = phoneH * 1.48f;
    const ImVec2 areaPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail.x, areaH));
    ui::DrawPhone(ImGui::GetWindowDrawList(), ImVec2(areaPos.x + avail.x * 0.5f, areaPos.y + phoneH * 0.5f + S(10)), phoneH,
        unauthorized ? ui::PhoneArt::Unauthorized : ui::PhoneArt::Waiting, t);

    VSpace(8);
    if (unauthorized) {
        {
            Font f(GetFonts().uiStrong, FontH2);
            ui::TextCentered("Check your phone and tap Allow");
        }
        VSpace(2);
        ui::TextCentered("Tick \"Always allow from this computer\" so you don't have to do this again.", &TextDim);
        VSpace(6);
        Font f(GetFonts().ui, FontSmall);
        ui::TextCentered("No prompt? Unplug and replug the cable, or revoke USB debugging authorizations in Developer options.", &TextDim);
    } else {
        {
            Font f(GetFonts().uiStrong, FontH2);
            ui::TextCentered("Plug in your phone with a USB cable");
        }
        VSpace(2);
        ui::TextCentered("Make sure USB debugging is on in Developer options.", &TextDim);
        VSpace(14);

        const char* label = m_showGuide ? ICON_CHEVRON_DOWN "  How do I turn on USB debugging?"
                                        : ICON_QUESTION "  How do I turn on USB debugging?";
        const float bw = ui::TextWidth(label) + S(28) + 2;
        ImGui::SetCursorPosX((avail.x - bw) * 0.5f);
        if (ui::SecondaryButton(label, ImVec2(bw, 0)))
            m_showGuide = !m_showGuide;

        if (m_showGuide) {
            VSpace(12);
            DrawUsbGuide();
        }
    }

    // Offline/other states (e.g. "offline", "recovery") get a one-line hint.
    for (const auto& e : ctx.devices.entries) {
        if (e.state == DeviceEntry::State::Offline || e.state == DeviceEntry::State::Other) {
            VSpace(10);
            const std::string msg = std::format(ICON_INFO "  A device is connected but reports \"{}\". "
                                                "Replugging usually fixes this.", e.rawState);
            ui::TextCentered(msg, &Warning);
            break;
        }
    }

    m_lastContentHeight = ImGui::GetCursorPosY() - startY;
}

void HomeScreen::DrawUsbGuide()
{
    const float w = std::min(S(520), ImGui::GetContentRegionAvail().x);
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
    ui::BeginCard("##guide", w);

    struct Step {
        const char* title;
        const char* detail;
    };
    static constexpr Step kSteps[] = {
        { "Open Settings \xE2\x86\x92 About phone \xE2\x86\x92 Software information",
            "On non-Samsung phones: Settings \xE2\x86\x92 About phone." },
        { "Tap \"Build number\" 7 times", "Enter your PIN if asked. You'll see \"Developer mode has been turned on\"." },
        { "Go back to Settings \xE2\x86\x92 Developer options",
            "It's at the very bottom of Settings (or under System on some phones)." },
        { "Turn on \"USB debugging\", then plug in the cable", "Tap Allow on the prompt that appears on the phone." },
    };

    for (int i = 0; i < 4; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = S(11);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, U32(Accent, 0.15f));
        {
            Font f(GetFonts().uiStrong, FontSmall);
            const std::string n = std::to_string(i + 1);
            const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(p.x + r - ts.x * 0.5f, p.y + r - ts.y * 0.5f), U32(Accent), n.c_str());
        }
        ImGui::Dummy(ImVec2(r * 2, r * 2));
        ImGui::SameLine(0, S(12));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(kSteps[i].title);
        {
            Font f(GetFonts().ui, FontSmall);
            ui::DimText(kSteps[i].detail);
        }
        ImGui::EndGroup();
        if (i < 3)
            VSpace(6);
    }
    ui::EndCard();
}

void HomeScreen::DrawReadyDevices(AppContext& ctx)
{
    const auto& snap = ctx.devices;
    std::vector<const DeviceEntry*> ready;
    int unauth = 0;
    for (const auto& e : snap.entries) {
        if (e.state == DeviceEntry::State::Device)
            ready.push_back(&e);
        unauth += e.state == DeviceEntry::State::Unauthorized;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPosY(std::max(S(8), (avail.y - m_lastContentHeight) * 0.5f));
    const float startY = ImGui::GetCursorPosY();

    auto loadingCard = [&](const DeviceEntry& e, float w) {
        ImGui::PushID(e.serial.c_str());
        ui::BeginCard("##loading", w);
        ui::Spinner(S(9), S(2), Accent);
        ImGui::SameLine(0, S(10));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(e.model.empty() ? "Phone" : e.model.c_str());
        Font f(GetFonts().ui, FontSmall);
        ui::DimText("Reading device info\xE2\x80\xA6");
        ImGui::EndGroup();
        // Skeleton rows so the card doesn't jump in height much when data lands.
        for (int i = 0; i < 4; ++i) {
            ImGui::Dummy(ImVec2(0, S(6)));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float a = 0.5f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f + i);
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w * (0.35f + 0.12f * (i % 3)), p.y + S(10)), U32(Border, a), S(4));
            ImGui::Dummy(ImVec2(0, S(10)));
        }
        ui::EndCard();
        ImGui::PopID();
    };

    if (ready.size() == 1) {
        // Phone shrinks and slides up; card fades in beneath it.
        const float k = m_readyAnim;
        const float phoneH = S(200) + (S(96) - S(200)) * k;
        const float areaH = phoneH * (1.0f + 0.48f * (1.0f - k)) + S(12);
        const ImVec2 areaPos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail.x, areaH));
        ui::DrawPhone(ImGui::GetWindowDrawList(), ImVec2(areaPos.x + avail.x * 0.5f, areaPos.y + phoneH * 0.5f + S(4)), phoneH,
            ui::PhoneArt::Ready, ImGui::GetTime());

        const float cardW = std::min(S(500), avail.x);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::clamp(k * 1.4f - 0.2f, 0.0f, 1.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - k) * S(16));
        ImGui::SetCursorPosX((avail.x - cardW) * 0.5f);
        const auto& e = *ready.front();
        if (const DeviceInfo* info = snap.FindInfo(e.serial)) {
            ui::DeviceCardOptions o;
            o.revealSerial = &m_revealSerial[e.serial];
            if (ui::DeviceCard(*info, cardW, o) == ui::DeviceCardAction::Proceed)
                Proceed(ctx, *info);
        } else {
            loadingCard(e, cardW);
        }
        ImGui::PopStyleVar();
    } else {
        {
            Font f(GetFonts().uiStrong, FontH2);
            ui::TextCentered(std::format("{} phones connected: pick one to work on.", ready.size()));
        }
        VSpace(18);

        const float gap = S(16);
        const size_t perRow = std::min<size_t>(3, ready.size());
        const float cardW = std::min(S(420), (avail.x - gap * (perRow - 1)) / perRow);
        for (size_t row = 0; row * perRow < ready.size(); ++row) {
            const size_t first = row * perRow, n = std::min(perRow, ready.size() - first);
            const float rowW = cardW * n + gap * (n - 1);
            ImGui::SetCursorPosX((avail.x - rowW) * 0.5f);
            const float rowY = ImGui::GetCursorPosY();
            float rowBottom = rowY;
            for (size_t i = 0; i < n; ++i) {
                ImGui::SetCursorPos(ImVec2((avail.x - rowW) * 0.5f + i * (cardW + gap), rowY));
                const auto& e = *ready[first + i];
                if (const DeviceInfo* info = snap.FindInfo(e.serial)) {
                    ui::DeviceCardOptions o;
                    o.showProceed = false;
                    o.clickable = true;
                    o.revealSerial = &m_revealSerial[e.serial];
                    if (ui::DeviceCard(*info, cardW, o) == ui::DeviceCardAction::Clicked)
                        Proceed(ctx, *info);
                } else {
                    loadingCard(e, cardW);
                }
                rowBottom = std::max(rowBottom, ImGui::GetCursorPosY());
            }
            ImGui::SetCursorPos(ImVec2(0, rowBottom));
            ImGui::Dummy(ImVec2(0, gap)); // an item, so the moved cursor counts toward window bounds
        }
    }

    if (unauth > 0) {
        VSpace(14);
        ui::TextCentered(std::format(ICON_INFO "  {} more phone{} connected but waiting for you to tap Allow on it.", unauth,
                             unauth > 1 ? "s are" : " is"),
            &Warning);
    }

    m_lastContentHeight = ImGui::GetCursorPosY() - startY;
}

void HomeScreen::Proceed(AppContext& ctx, const DeviceInfo& info)
{
    ctx.activeSerial = info.serial;
    ctx.activeInfo = info;
    ctx.page = Page::Overview;
    ctx.Navigate(Route::Workspace);
}

} // namespace ps
