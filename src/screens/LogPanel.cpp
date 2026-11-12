#include "screens/LogPanel.h"

#include "app/AppContext.h"
#include "platform/Platform.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <chrono>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

bool ContainsCaseInsensitive(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
        return true;
    auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

ImVec4 LevelColor(LogLevel l)
{
    switch (l) {
    case LogLevel::Warn: return Warning;
    case LogLevel::Error: return Danger;
    case LogLevel::AdbCmd: return Info;
    default: return TextDim;
    }
}

std::string FormatLine(const LogEntry& e)
{
    const auto local = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(e.time));
    return std::format("{:%H:%M:%S} {:<5} {}", local, ToString(e.level), e.text);
}

} // namespace

bool LogPanel::Passes(const LogEntry& e) const
{
    switch (m_filter) {
    case Info: if (e.level != LogLevel::Info) return false; break;
    case Warn: if (e.level != LogLevel::Warn) return false; break;
    case Error: if (e.level != LogLevel::Error) return false; break;
    case Adb: if (e.level != LogLevel::AdbCmd) return false; break;
    default: break;
    }
    return ContainsCaseInsensitive(e.text, m_search);
}

void LogPanel::Draw(AppContext& ctx)
{
    m_next = Log::Get().Snapshot(m_next, m_entries);
    if (m_entries.size() > 20000)
        m_entries.erase(m_entries.begin(), m_entries.begin() + (m_entries.size() - 20000));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, Panel);
    const bool open = ImGui::Begin(kWindowName, &ctx.showLog, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }

    // Toolbar: filter chips, search, copy.
    static constexpr const char* kNames[] = { "All", "Info", "Warn", "Error", "ADB cmd" };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(3)));
    for (int i = 0; i < 5; ++i) {
        const bool active = m_filter == i;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? Hex(0x3DD6C6, 0.18f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Accent : TextDim);
        if (ImGui::Button(kNames[i]))
            m_filter = static_cast<Filter>(i);
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0, S(4));
    }
    ImGui::SameLine(0, S(12));
    ImGui::SetNextItemWidth(S(220));
    ImGui::InputTextWithHint("##logsearch", ICON_SEARCH "  Filter text", m_search, sizeof(m_search));
    ImGui::SameLine();
    if (ui::GhostButton(ICON_COPY "  Copy")) {
        std::string all;
        for (const auto& e : m_entries)
            if (Passes(e))
                all += FormatLine(e) + "\r\n";
        ctx.platform.SetClipboardText(all);
        ui::Toast("Copied to clipboard");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_autoScroll);
    ImGui::PopStyleVar();

    ImGui::BeginChild("##loglines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        Font f(GetFonts().mono, FontSmall);
        std::vector<int> visible;
        visible.reserve(m_entries.size());
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
            if (Passes(m_entries[i]))
                visible.push_back(i);

        if (visible.empty())
            ui::DimText(m_entries.empty() ? "Nothing logged yet." : "No lines match the filter.");

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& e = m_entries[visible[row]];
                const auto local = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(e.time));
                ui::DimText(std::format("{:%H:%M:%S}", local));
                ImGui::SameLine();
                ui::ColoredText(LevelColor(e.level), std::format("{:<5}", ToString(e.level)));
                ImGui::SameLine();
                ImGui::TextUnformatted(e.text.c_str());
            }
        }
        if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(40))
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ps
