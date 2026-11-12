#include "screens/WirelessDialog.h"

#include "adb/AdbClient.h"
#include "app/AppContext.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/JobSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace ps {

using namespace ps::theme;

void WirelessDialog::Remember(AppContext& ctx, const std::string& address, const std::string& name)
{
    auto& known = ctx.settings.wirelessKnown;
    auto it = std::ranges::find(known, address, &std::pair<std::string, std::string>::first);
    if (it == known.end()) {
        known.emplace_back(address, name);
    } else if (!name.empty() && it->second != name) {
        it->second = name;
    } else {
        return;
    }
    ctx.settings.Save();
}

void WirelessDialog::Run(AppContext& ctx, std::string action, std::function<WirelessResult(std::stop_token)> fn, bool pair)
{
    {
        std::lock_guard l(m_shared->mutex);
        if (m_shared->busy)
            return;
        m_shared->busy = true;
        m_shared->action = action;
        m_shared->result.reset();
        m_shared->pairStep = pair;
    }
    m_last.reset();
    auto shared = m_shared;
    ctx.jobs.Submit(std::move(action), JobPriority::High, [shared, fn = std::move(fn)](Job&, std::stop_token st) {
        auto r = fn(st);
        std::lock_guard l(shared->mutex);
        shared->result = std::move(r);
        shared->busy = false;
    });
}

void WirelessDialog::Draw(AppContext& ctx)
{
    if (!ctx.showWireless) {
        m_wasOpen = false;
        return;
    }
    if (!m_wasOpen) {
        ImGui::OpenPopup("Connect over Wi-Fi");
        m_wasOpen = true;
    }

    // Collect a finished action.
    bool busy;
    std::string action;
    {
        auto keep = m_shared;
        std::lock_guard l(keep->mutex);
        busy = keep->busy;
        action = keep->action;
        if (keep->result) {
            m_last = std::move(keep->result);
            keep->result.reset();
            if (m_last->ok && !keep->pairStep) {
                Remember(ctx, m_last->address, {});
                ui::Toast("Connected over Wi-Fi: " + m_last->address);
            } else if (m_last->ok && keep->pairStep) {
                // Paired: the connect port is a different one, shown on the phone's Wireless debugging screen.
                const std::string host = m_last->address.substr(0, m_last->address.rfind(':'));
                strncpy_s(m_address, host.c_str(), _TRUNCATE);
            }
        }
    }
    // Name known phones as soon as their info arrives.
    for (const auto& [serial, info] : ctx.devices.info)
        if (info.wireless)
            for (auto& [addr, name] : ctx.settings.wirelessKnown)
                if (addr == serial && name != info.displayName) {
                    name = info.displayName;
                    ctx.settings.Save();
                }

    const ImVec2 size(std::min(S(620), ImGui::GetMainViewport()->WorkSize.x - S(40)), 0);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("Connect over Wi-Fi", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ctx.showWireless = false;
        return;
    }
    const bool canAct = ctx.adb && !busy;
    auto section = [](const char* title) {
        ImGui::Dummy(ImVec2(0, S(6)));
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(title);
    };
    auto hint = [](std::string_view text) {
        Font f(GetFonts().ui, FontSmall);
        ImGui::PushTextWrapPos(0);
        ui::DimText(text);
        ImGui::PopTextWrapPos();
    };

    // 1. Phones on USB -> Wi-Fi.
    section(ICON_PLUG "  From USB to Wi-Fi (any Android)");
    hint("The phone and this PC must be on the same Wi-Fi. After switching, you can unplug the cable. "
         "The phone goes back to USB-only mode when it restarts.");
    int usb = 0;
    for (const auto& e : ctx.devices.entries) {
        if (e.state != DeviceEntry::State::Device)
            continue;
        const auto it = ctx.devices.info.find(e.serial);
        if (it == ctx.devices.info.end() || it->second.wireless)
            continue;
        ++usb;
        const DeviceInfo& info = it->second;
        ImGui::PushID(e.serial.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(std::format(ICON_PHONE "  {}  \xC2\xB7  Android {}", info.displayName, info.android).c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ui::TextWidth(ICON_WIFI "  Switch to Wi-Fi") - S(30)));
        if (ui::PrimaryButton(ICON_WIFI "  Switch to Wi-Fi", {}, canAct)) {
            auto adb = ctx.adb;
            const std::string serial = e.serial;
            Run(ctx, "Switching " + info.displayName + " to Wi-Fi\xE2\x80\xA6", [adb, serial](std::stop_token st) { return SwitchToWifi(*adb, serial, st); });
        }
        ImGui::PopID();
    }
    if (usb == 0)
        hint("No phone on USB right now.");

    // 2. Address.
    section(ICON_WIFI "  Connect to an address");
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputTextWithHint("##addr", "192.168.0.3 or 192.168.0.3:37199", m_address, sizeof(m_address));
    ImGui::SameLine();
    const auto addr = NormalizeAddress(m_address);
    if (ui::SecondaryButton("Connect##addr", {}, canAct && addr.has_value())) {
        auto adb = ctx.adb;
        const std::string a = *addr;
        Run(ctx, "Connecting to " + a + "\xE2\x80\xA6", [adb, a](std::stop_token st) { return ConnectWireless(*adb, a, st); });
    }
    hint("Android 11+: use the IP address & port shown under Developer options \xE2\x86\x92 Wireless debugging (pair first, below).");

    // 3. Pair.
    section(ICON_LOCK "  Pair with a code (Android 11+)");
    hint("On the phone: Wireless debugging \xE2\x86\x92 Pair device with pairing code. Type the IP address & port and the 6-digit code it shows.");
    ImGui::SetNextItemWidth(S(220));
    ImGui::InputTextWithHint("##paddr", "192.168.0.3:41234", m_pairAddress, sizeof(m_pairAddress));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(110));
    ImGui::InputTextWithHint("##pcode", "123456", m_pairCode, sizeof(m_pairCode), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    const auto paddr = std::strchr(m_pairAddress, ':') ? NormalizeAddress(m_pairAddress) : std::nullopt; // pairing port is required
    if (ui::SecondaryButton("Pair", {}, canAct && paddr.has_value() && std::strlen(m_pairCode) == 6)) {
        auto adb = ctx.adb;
        const std::string a = *paddr, code = m_pairCode;
        Run(ctx, "Pairing with " + a + "\xE2\x80\xA6", [adb, a, code](std::stop_token st) { return PairWireless(*adb, a, code, st); }, true);
    }

    // 4. Known phones.
    if (!ctx.settings.wirelessKnown.empty()) {
        section(ICON_CLOCK_ROTATE "  Phones used over Wi-Fi before");
        std::optional<size_t> forget;
        for (size_t i = 0; i < ctx.settings.wirelessKnown.size(); ++i) {
            const auto& [a, name] = ctx.settings.wirelessKnown[i];
            const DeviceEntry* e = ctx.devices.Find(a);
            const bool connected = e && e->state == DeviceEntry::State::Device;
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ui::ColoredText(connected ? Success : TextDim, connected ? ICON_CIRCLE_CHECK : ICON_WIFI);
            ImGui::SameLine();
            ImGui::TextUnformatted(name.empty() ? a.c_str() : std::format("{}  \xC2\xB7  {}", name, a).c_str());
            ImGui::SameLine();
            const char* btn = connected ? "Disconnect" : "Reconnect";
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ui::TextWidth(btn) - ui::TextWidth("Forget") - S(60)));
            if (ui::SecondaryButton(btn, {}, canAct)) {
                auto adb = ctx.adb;
                const std::string address = a;
                if (connected)
                    Run(ctx, "Disconnecting\xE2\x80\xA6", [adb, address](std::stop_token) {
                        DisconnectWireless(*adb, address);
                        return WirelessResult{ true, "", "Disconnected " + address };
                    }, true); // pairStep=true: don't "remember" a disconnect
                else
                    Run(ctx, "Connecting to " + address + "\xE2\x80\xA6", [adb, address](std::stop_token st) { return ConnectWireless(*adb, address, st); });
            }
            ImGui::SameLine();
            if (ui::GhostButton("Forget"))
                forget = i;
            ImGui::PopID();
        }
        if (forget) {
            ctx.settings.wirelessKnown.erase(ctx.settings.wirelessKnown.begin() + static_cast<long>(*forget));
            ctx.settings.Save();
        }
        if (ImGui::Checkbox("Reconnect these when PhoneSweep starts", &ctx.settings.wirelessReconnect))
            ctx.settings.Save();
    }

    // Status.
    ImGui::Dummy(ImVec2(0, S(8)));
    ui::HLine();
    ImGui::Dummy(ImVec2(0, S(4)));
    if (busy) {
        ui::Spinner(S(7), S(2), Accent);
        ImGui::SameLine(0, S(8));
        ui::DimText(action);
    } else if (m_last) {
        ImGui::PushTextWrapPos(0);
        ui::ColoredText(m_last->ok ? Success : Warning, std::string(m_last->ok ? ICON_CIRCLE_CHECK "  " : ICON_WARNING "  ") + m_last->message
                + (m_last->ok && !m_last->address.empty() ? "  (" + m_last->address + ")" : ""));
        ImGui::PopTextWrapPos();
    } else if (!ctx.adb) {
        ui::ColoredText(Warning, ICON_WARNING "  adb isn't available.");
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    if (ui::SecondaryButton("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape) || !open) {
        ctx.showWireless = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace ps
