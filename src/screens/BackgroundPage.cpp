#include "screens/BackgroundPage.h"

#include "adb/AdbClient.h"
#include "app/AppContext.h"
#include "scan/AppSession.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "util/JobSystem.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

const AppInfo* FindApp(const AppSession& apps, std::string_view pkg)
{
    for (const auto& a : apps.Apps())
        if (a.package == pkg)
            return &a;
    return nullptr;
}

// Two-line app cell: name (label or last package segment) + package in mono.
void AppCell(ImDrawList* dl, ImVec2 p, float rowH, std::string_view pkg, const AppInfo* app)
{
    const float lh = ImGui::GetTextLineHeight();
    const float y0 = p.y + (rowH - lh * 2 - S(2)) * 0.5f;
    std::string name = app && !app->label.empty() ? app->label : std::string(pkg.substr(pkg.rfind('.') + 1));
    {
        Font f(GetFonts().uiStrong, FontBase);
        dl->AddText(ImVec2(p.x, y0), U32(app && !app->label.empty() ? Text : TextDim), name.c_str());
        if (app && app->risk >= 20) {
            const float w = ImGui::CalcTextSize(name.c_str()).x;
            dl->AddText(ImVec2(p.x + w + S(6), y0), U32(Warning), ICON_WARNING);
        }
    }
    Font f(GetFonts().mono, FontSmall);
    dl->AddText(ImVec2(p.x, y0 + lh + S(2)), U32(TextDim), pkg.data(), pkg.data() + pkg.size());
}

void CellText(float rowH, const std::string& s, const ImVec4& col)
{
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
    ui::ColoredText(col, s);
}

} // namespace

bool BackgroundPage::Busy(std::string_view pkg)
{
    std::lock_guard l(m_shared->mutex);
    return m_shared->busy.contains(pkg);
}

void BackgroundPage::LoadStates(AppContext& ctx, std::vector<std::string> packages)
{
    if (!ctx.adb || !ctx.ActiveConnected() || packages.empty())
        return;
    {
        std::lock_guard l(m_shared->mutex);
        if (m_shared->statesLoading)
            return;
        m_shared->statesLoading = true;
    }
    auto shared = m_shared;
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial;
    ctx.jobs.Submit("Reading background state", JobPriority::Normal, [=](Job& job, std::stop_token st) {
        job.SetDetail(std::format("{} apps", packages.size()));
        auto states = QueryBackground(*adb, serial, packages, st);
        std::lock_guard l(shared->mutex);
        for (auto& [k, v] : states)
            shared->states[k] = v;
        shared->statesLoading = false;
        shared->statesLoaded = true;
    });
}

void BackgroundPage::LoadRunning(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    {
        std::lock_guard l(m_shared->mutex);
        if (m_shared->runningLoading)
            return;
        m_shared->runningLoading = true;
    }
    auto shared = m_shared;
    auto adb = ctx.adb;
    auto* usage = &ctx.usage;
    const std::string serial = ctx.activeSerial;
    // Low priority and quiet: this runs every 2 s while the tab is open.
    ctx.jobs.Submit("Sampling CPU/RAM", JobPriority::Low, [=](Job&, std::stop_token st) {
        // Two frames 1 s apart: the second has real CPU% over that second.
        const auto r = adb->Shell(serial, ShellCommand("top").arg("-b").arg("-n").arg("2").arg("-d").arg("1").arg("-o").arg("PID,USER,%CPU,RES,NAME"),
            { .timeout = 20s, .cancel = st });
        const auto sample = ParseTop(r.out);
        auto live = GroupTop(sample);
        if (!sample.processes.empty())
            usage->SetLive(live);
        std::lock_guard l(shared->mutex);
        shared->runningLoading = false;
        shared->runningLoaded = true;
        if (sample.processes.empty()) {
            shared->runningError = r.err.empty() ? (r.out.empty() ? std::string("top printed nothing") : std::string(r.out.substr(0, 200))) : r.err;
            return;
        }
        shared->runningError.clear();
        shared->live = std::move(live);
        shared->cores = sample.cores;
        ++shared->liveSeq;
    });
}

void BackgroundPage::LoadBattery(AppContext& ctx)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    {
        std::lock_guard l(m_shared->mutex);
        if (m_shared->batteryLoading)
            return;
        m_shared->batteryLoading = true;
    }
    auto shared = m_shared;
    auto adb = ctx.adb;
    const std::string serial = ctx.activeSerial;
    ctx.jobs.Submit("Reading battery stats", JobPriority::Normal, [=](Job&, std::stop_token st) {
        const auto r = adb->Shell(serial, ShellCommand("dumpsys").arg("batterystats").arg("--charged"), { .timeout = 90s, .cancel = st });
        auto drain = ParseBatteryStats(r.out);
        std::lock_guard l(shared->mutex);
        shared->batteryLoading = false;
        shared->batteryLoaded = true;
        if (!drain.found) {
            shared->batteryError = r.err.empty() ? std::string("This phone's batterystats has no \"Estimated power use\" section yet (it appears after some use since the last full charge).") : r.err;
            return;
        }
        shared->batteryError.clear();
        shared->drain = std::move(drain);
    });
}

void BackgroundPage::Restrict(AppContext& ctx, const std::vector<std::string>& packages)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    auto shared = m_shared;
    auto adb = ctx.adb;
    auto* log = &ctx.restrictions;
    const std::string serial = ctx.activeSerial;
    const std::string deviceId = ctx.DeviceId();
    const int sdk = ctx.activeInfo.sdk;
    {
        std::lock_guard l(shared->mutex);
        for (const auto& p : packages)
            shared->busy.insert(p);
    }
    ctx.jobs.Submit("Restricting background", JobPriority::High, [=](Job& job, std::stop_token st) {
        for (const auto& pkg : packages) {
            job.SetDetail(pkg);
            auto r = RestrictBackground(*adb, serial, pkg, sdk, st);
            r.record.serial = deviceId; // keyed by the phone, not the USB/Wi-Fi link
            if (r.ok)
                log->Add(r.record);
            const auto after = QueryBackground(*adb, serial, { pkg }, st);
            std::lock_guard l(shared->mutex);
            if (auto it = after.find(pkg); it != after.end())
                shared->states[pkg] = it->second;
            shared->busy.erase(pkg);
            shared->results.emplace_back(pkg, r.ok ? std::string("Restricted") : r.error, r.ok);
        }
    });
}

void BackgroundPage::Undo(AppContext& ctx, const std::vector<std::string>& packages)
{
    if (!ctx.adb || !ctx.ActiveConnected())
        return;
    auto shared = m_shared;
    auto adb = ctx.adb;
    auto* log = &ctx.restrictions;
    const std::string serial = ctx.activeSerial;
    const std::string deviceId = ctx.DeviceId();
    {
        std::lock_guard l(shared->mutex);
        for (const auto& p : packages)
            shared->busy.insert(p);
    }
    ctx.jobs.Submit("Undoing restriction", JobPriority::High, [=](Job& job, std::stop_token st) {
        for (const auto& pkg : packages) {
            job.SetDetail(pkg);
            std::string err;
            bool ok = false;
            if (const auto rec = log->Find(deviceId, pkg)) {
                ok = UndoRestriction(*adb, serial, *rec, &err, st);
                if (ok)
                    log->Remove(deviceId, pkg);
            } else {
                err = "No record of restricting this app";
            }
            const auto after = QueryBackground(*adb, serial, { pkg }, st);
            std::lock_guard l(shared->mutex);
            if (auto it = after.find(pkg); it != after.end())
                shared->states[pkg] = it->second;
            shared->busy.erase(pkg);
            shared->results.emplace_back(pkg, ok ? std::string("Restriction undone") : err, ok);
        }
    });
}

void BackgroundPage::Draw(AppContext& ctx)
{
    // New device: start over.
    if (ctx.activeSerial != m_serial) {
        m_serial = ctx.activeSerial;
        m_shared = std::make_shared<Shared>();
        m_selected.clear();
        m_appsVersion = ~0ull;
        m_history.clear();
        m_seenSeq = 0;
        m_lastPoll = -100.0;
    }

    if (ctx.filesPreset == "running") { // dev flag --files=running
        ctx.filesPreset.clear();
        m_tab = 1;
    } else if (ctx.filesPreset == "battery") {
        ctx.filesPreset.clear();
        m_tab = 2;
    }

    // Report finished actions.
    std::vector<std::tuple<std::string, std::string, bool>> results;
    {
        std::lock_guard l(m_shared->mutex);
        results.swap(m_shared->results);
    }
    if (results.size() == 1) {
        const auto& [pkg, msg, ok] = results.front();
        ui::Toast(std::format("{}: {}", msg, pkg), ok ? ui::ToastKind::Success : ui::ToastKind::Error);
    } else if (!results.empty()) {
        const auto okCount = std::ranges::count_if(results, [](const auto& r) { return std::get<2>(r); });
        ui::Toast(std::format("{} of {} done", okCount, results.size()),
            okCount == static_cast<long long>(results.size()) ? ui::ToastKind::Success : ui::ToastKind::Warning);
    }

    // Tabs.
    auto tab = [&](int idx, const char* label, bool enabled, const char* tip = nullptr) {
        const bool active = m_tab == idx;
        if (!enabled)
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, active ? PanelRaised : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PanelHover);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Text : TextDim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(7)));
        if (ImGui::Button(label) && !active)
            m_tab = idx;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (!enabled)
            ImGui::EndDisabled();
        if (tip)
            ImGui::SetItemTooltip("%s", tip);
        if (active) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + S(8), b.y - S(2)), ImVec2(b.x - S(8), b.y), U32(Accent), S(1));
        }
    };
    tab(0, ICON_POWER "  Starts at boot", true);
    ImGui::SameLine(0, S(4));
    tab(1, ICON_GAUGE "  Running now", true);
    ImGui::SameLine(0, S(4));
    tab(2, ICON_BOLT "  Battery", true);
    ImGui::Dummy(ImVec2(0, S(6)));

    const float h = ImGui::GetContentRegionAvail().y;
    if (ctx.apps.Apps().empty()) {
        if (ctx.apps.Running())
            ui::SkeletonRows(std::max(3, static_cast<int>(h / S(52))), S(52));
        else
            ui::EmptyState(ICON_CUBES, "Scan apps first", "Background & Startup uses the app scan. Rescan from Overview.");
        return;
    }
    if (m_tab == 0)
        DrawBootTab(ctx, h);
    else if (m_tab == 1)
        DrawRunningTab(ctx, h);
    else
        DrawBatteryTab(ctx, h);
}

void BackgroundPage::DrawBootTab(AppContext& ctx, float /*height*/)
{
    const AppSession& apps = ctx.apps;

    // Rows: apps registered for boot.
    std::vector<const AppInfo*> rows;
    for (const auto& a : apps.Apps())
        if (a.Has(AppFlag_Boot) && (m_includeSystem || !a.system))
            rows.push_back(&a);
    std::ranges::sort(rows, [](const AppInfo* x, const AppInfo* y) {
        if (x->risk != y->risk)
            return x->risk > y->risk;
        return x->DisplayName() < y->DisplayName();
    });

    // (Re)load states for the rows when the app list changes.
    bool loaded, loading;
    {
        std::lock_guard l(m_shared->mutex);
        loaded = m_shared->statesLoaded;
        loading = m_shared->statesLoading;
    }
    if ((!loaded || apps.Version() != m_appsVersion) && !loading) {
        std::vector<std::string> pkgs;
        for (const AppInfo* a : rows)
            pkgs.push_back(a->package);
        // System apps are only queried when shown.
        LoadStates(ctx, std::move(pkgs));
        m_appsVersion = apps.Version();
    }

    // Info banner.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool oldAndroid = ctx.activeInfo.sdk > 0 && ctx.activeInfo.sdk < 31;
        const std::string text = std::string(ICON_INFO "  Restricting stops an app from running in the background. Its notifications may arrive late "
                                             "until you open it. Every change can be undone here or in Settings \xE2\x86\x92 Safety.")
            + (oldAndroid ? std::format("\n      Android {} has no \"restricted\" standby bucket, so PhoneSweep uses \"rare\" (the lowest it allows).",
                                ctx.activeInfo.android)
                          : std::string());
        Font f(GetFonts().ui, FontSmall);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, w - S(24));
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + ts.y + S(16)), U32(Info, 0.10f), S(RoundButton));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + S(8)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - S(24));
        ui::ColoredText(Mix(Info, Text, 0.4f), text);
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ts.y + S(16)));
        ImGui::Dummy(ImVec2(w, S(6)));
    }

    // Toolbar.
    const size_t restrictedHere = ctx.restrictions.ForDevice(ctx.DeviceId()).size();
    ImGui::AlignTextToFramePadding();
    ui::DimText(std::format("{} apps start at boot{}", rows.size(),
        restrictedHere ? std::format("  \xC2\xB7  {} restricted by PhoneSweep", restrictedHere) : std::string()));
    ImGui::SameLine();
    const char* sysLabel = "Show system apps";
    const float cbW = ImGui::GetFrameHeight() + ui::TextWidth(sysLabel) + S(8);
    const float refreshW = ui::TextWidth(ICON_ROTATE "  Refresh") + S(28);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - cbW - refreshW - S(12)));
    if (ImGui::Checkbox(sysLabel, &m_includeSystem))
        m_appsVersion = ~0ull; // re-query with the new set
    ImGui::SameLine(0, S(12));
    if (ui::SecondaryButton(ICON_ROTATE "  Refresh", ImVec2(refreshW, 0), ctx.ActiveConnected() && !loading))
        m_appsVersion = ~0ull;

    // Table.
    const bool hasSel = !m_selected.empty();
    const float tableH = ImGui::GetContentRegionAvail().y - (hasSel ? S(54) : 0.0f);
    if (rows.empty()) {
        ImGui::BeginChild("##bootempty", ImVec2(0, tableH));
        ui::EmptyState(ICON_POWER, "No apps start at boot", m_includeSystem ? "" : "Tick \"Show system apps\" to include them.");
        ImGui::EndChild();
    } else {
        const float rowH = S(52);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8), 0));
        if (ImGui::BeginTable("##boot", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed, S(26));
            ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Boot receiver", ImGuiTableColumnFlags_WidthFixed, S(110));
            ImGui::TableSetupColumn("Background", ImGuiTableColumnFlags_WidthFixed, S(110));
            ImGui::TableSetupColumn("Standby bucket", ImGuiTableColumnFlags_WidthFixed, S(130));
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, S(120));
            ImGui::TableHeadersRow();

            std::map<std::string, BgState, std::less<>> states;
            {
                std::lock_guard l(m_shared->mutex);
                states = m_shared->states;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()), rowH);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const AppInfo& a = *rows[i];
                    const bool selected = m_selected.contains(a.package);
                    const auto rec = ctx.restrictions.Find(ctx.DeviceId(), a.package);
                    const auto st = states.find(a.package);
                    const bool busy = Busy(a.package);
                    ImGui::TableNextRow(0, rowH);
                    ImGui::PushID(a.package.c_str());

                    ImGui::TableSetColumnIndex(0);
                    {
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const float box = S(15);
                        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (rowH - box) * 0.5f));
                        if (ImGui::InvisibleButton("##sel", ImVec2(box, box)) && !a.system) {
                            if (selected)
                                m_selected.erase(a.package);
                            else
                                m_selected.insert(a.package);
                        }
                        const ImVec2 bp = ImGui::GetItemRectMin();
                        if (a.system)
                            dl->AddRect(bp, ImVec2(bp.x + box, bp.y + box), U32(Border), S(3));
                        else if (selected) {
                            dl->AddRectFilled(bp, ImVec2(bp.x + box, bp.y + box), U32(Accent), S(3));
                            dl->AddText(ImVec2(bp.x + S(1), bp.y - S(1)), U32(Background), ICON_CHECK);
                        } else
                            dl->AddRect(bp, ImVec2(bp.x + box, bp.y + box), U32(TextDim, 0.7f), S(3), 0, 1.2f);
                    }

                    ImGui::TableSetColumnIndex(1);
                    AppCell(dl, ImGui::GetCursorScreenPos(), rowH, a.package, &a);
                    ImGui::Dummy(ImVec2(0, rowH));

                    ImGui::TableSetColumnIndex(2);
                    CellText(rowH, ICON_CHECK, Success);

                    ImGui::TableSetColumnIndex(3);
                    if (st == states.end() || busy) {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - S(12)) * 0.5f);
                        ui::Spinner(S(6), S(1.5f), TextDim);
                    } else {
                        const bool allowed = st->second.BackgroundAllowed();
                        CellText(rowH, allowed ? "Yes" : "No", allowed ? Text : Success);
                    }

                    ImGui::TableSetColumnIndex(4);
                    if (st != states.end() && !busy) {
                        const int b = st->second.bucket;
                        CellText(rowH, BucketName(b), b >= 40 ? Success : b > 0 && b <= 10 ? Warning : Text);
                    }

                    ImGui::TableSetColumnIndex(5);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetFrameHeight()) * 0.5f);
                    if (a.system) {
                        ui::DimText(ICON_LOCK " system");
                    } else if (rec) {
                        if (ui::SecondaryButton("Undo", ImVec2(S(100), 0), !busy && ctx.ActiveConnected()))
                            Undo(ctx, { a.package });
                        ImGui::SetItemTooltip("Restricted %s. Undo restores mode '%s' and bucket %s.", rec->appliedAt.c_str(),
                            rec->before.explicitMode ? rec->before.mode.c_str() : "default", BucketName(rec->before.bucket));
                    } else {
                        if (ui::SecondaryButton("Restrict", ImVec2(S(100), 0), !busy && ctx.ActiveConnected()))
                            Restrict(ctx, { a.package });
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    // Selection bar.
    if (hasSel) {
        std::vector<std::string> toRestrict, toUndo;
        for (const auto& p : m_selected)
            (ctx.restrictions.Find(ctx.DeviceId(), p) ? toUndo : toRestrict).push_back(p);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelRaised);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), 0));
        ImGui::BeginChild("##bootsel", ImVec2(0, S(50)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::SetCursorPosY((S(50) - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::AlignTextToFramePadding();
        {
            Font f(GetFonts().uiStrong, FontBase);
            ImGui::TextUnformatted(std::format("{} selected", m_selected.size()).c_str());
        }
        const std::string rl = std::format("Restrict {}", toRestrict.size());
        const std::string ul = std::format("Undo {}", toUndo.size());
        const float total = ui::TextWidth(rl) + ui::TextWidth(ul) + ui::TextWidth("Clear") + S(28) * 3 + S(16);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));
        if (ui::PrimaryButton(rl.c_str(), {}, !toRestrict.empty() && ctx.ActiveConnected())) {
            Restrict(ctx, toRestrict);
            m_selected.clear();
        }
        ImGui::SameLine(0, S(8));
        if (ui::SecondaryButton(ul.c_str(), {}, !toUndo.empty() && ctx.ActiveConnected())) {
            Undo(ctx, toUndo);
            m_selected.clear();
        }
        ImGui::SameLine(0, S(8));
        if (ui::GhostButton("Clear"))
            m_selected.clear();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}


namespace {

// Tiny line chart of the last samples (0..maxValue), newest at the right.
void Sparkline(ImDrawList* dl, ImVec2 p, ImVec2 size, const std::deque<float>& v, float maxValue, const ImVec4& col)
{
    if (v.size() < 2 || maxValue <= 0)
        return;
    std::vector<ImVec2> pts;
    pts.reserve(v.size());
    const float step = size.x / 29.0f; // 30 samples across
    const float x0 = p.x + size.x - step * static_cast<float>(v.size() - 1);
    for (size_t i = 0; i < v.size(); ++i)
        pts.emplace_back(x0 + step * static_cast<float>(i), p.y + size.y - size.y * std::clamp(v[i] / maxValue, 0.0f, 1.0f));
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), U32(col, 0.9f), ImDrawFlags_None, S(1.5f));
}

} // namespace

void BackgroundPage::DrawRunningTab(AppContext& ctx, float)
{
    bool loaded, loading;
    std::string error;
    std::vector<LiveApp> live;
    int cores;
    uint64_t seq;
    {
        std::lock_guard l(m_shared->mutex);
        loaded = m_shared->runningLoaded;
        loading = m_shared->runningLoading;
        error = m_shared->runningError;
        live = m_shared->live;
        cores = m_shared->cores;
        seq = m_shared->liveSeq;
    }
    // New sample -> extend the sparkline histories.
    if (seq != m_seenSeq) {
        m_seenSeq = seq;
        std::set<std::string, std::less<>> seen;
        for (const auto& a : live) {
            auto& h = m_history[a.package];
            h.cpu.push_back(static_cast<float>(a.cpu));
            h.ram.push_back(static_cast<float>(a.ramBytes));
            seen.insert(a.package);
        }
        std::erase_if(m_history, [&](const auto& kv) { return !seen.contains(kv.first); });
        for (auto& [k, h] : m_history) {
            while (h.cpu.size() > 30)
                h.cpu.pop_front();
            while (h.ram.size() > 30)
                h.ram.pop_front();
        }
    }
    // Poll every 2 s, only while this tab is on screen (Draw isn't called otherwise).
    const double now = ImGui::GetTime();
    if (!m_livePaused && !loading && ctx.ActiveConnected() && now - m_lastPoll >= 2.0) {
        m_lastPoll = now;
        LoadRunning(ctx);
    }

    if (m_liveSort == 1)
        std::ranges::stable_sort(live, [](const LiveApp& a, const LiveApp& b) { return a.ramBytes > b.ramBytes; });
    std::vector<const LiveApp*> heavy;
    for (const auto& a : live)
        if (IsHeavy(a))
            heavy.push_back(&a);

    // Toolbar.
    ImGui::AlignTextToFramePadding();
    if (m_livePaused)
        ui::ColoredText(TextDim, ICON_POWER "  Paused");
    else
        ui::ColoredText(Success, "\xE2\x97\x8F  Live");
    ImGui::SameLine(0, S(10));
    ui::DimText(loaded ? std::format("{} apps \xC2\xB7 every 2 s \xC2\xB7 CPU: 100% = one full core{}", live.size(),
                             cores > 0 ? std::format(" (this phone has {})", cores) : std::string())
                       : std::string("Reading processes\xE2\x80\xA6"));
    ImGui::SameLine();
    const char* sorts[] = { "Sort: CPU", "Sort: RAM" };
    const char* pauseLabel = m_livePaused ? ICON_ROTATE "  Resume" : ICON_POWER "  Pause";
    const float pauseW = ui::TextWidth(pauseLabel) + S(28);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - pauseW - S(130) - S(8)));
    ImGui::SetNextItemWidth(S(130));
    if (ImGui::BeginCombo("##livesort", sorts[m_liveSort])) {
        for (int i = 0; i < 2; ++i)
            if (ImGui::Selectable(sorts[i], m_liveSort == i))
                m_liveSort = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, S(8));
    if (ui::SecondaryButton(pauseLabel, ImVec2(pauseW, 0)))
        m_livePaused = !m_livePaused;

    if (!error.empty() && !loaded) {
        if (ui::ErrorCard("toperr", "Couldn't read CPU/RAM from the phone.", error))
            LoadRunning(ctx);
        return;
    }
    const float h0 = ImGui::GetContentRegionAvail().y;
    if (!loaded) {
        ui::SkeletonRows(std::max(3, static_cast<int>(h0 / S(52))), S(52));
        return;
    }

    // "Heavy right now".
    if (!heavy.empty()) {
        ImGui::Dummy(ImVec2(0, S(2)));
        ui::Pill(std::format(ICON_FIRE " HEAVY RIGHT NOW: {}", heavy.size()), Warning, true);
        for (size_t i = 0; i < std::min<size_t>(heavy.size(), 4); ++i) {
            ImGui::SameLine(0, S(8));
            const AppInfo* app = FindApp(ctx.apps, heavy[i]->package);
            ui::Pill(app && !app->label.empty() ? app->label : heavy[i]->package, Warning);
        }
        ImGui::SameLine(0, S(8));
        ImGui::AlignTextToFramePadding();
        ui::DimText("> 15% CPU or > 500 MB RAM");
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    const float h = ImGui::GetContentRegionAvail().y;
    const float rowH = S(52);
    float maxCpu = 50.0f, maxRam = 1;
    for (const auto& [k, hist] : m_history) {
        for (float v : hist.cpu)
            maxCpu = std::max(maxCpu, v);
        for (float v : hist.ram)
            maxRam = std::max(maxRam, v);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8), 0));
    if (ImGui::BeginTable("##live", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, S(170));
        ImGui::TableSetupColumn("RAM", ImGuiTableColumnFlags_WidthFixed, S(170));
        ImGui::TableSetupColumn("Proc.", ImGuiTableColumnFlags_WidthFixed, S(50));
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, S(130));
        ImGui::TableHeadersRow();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(live.size()), rowH);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const LiveApp& r = live[i];
                const AppInfo* app = FindApp(ctx.apps, r.package);
                const bool system = !app || app->system;
                ImGui::TableNextRow(0, rowH);
                if (IsHeavy(r))
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, U32(Warning, 0.07f));
                ImGui::PushID(r.package.c_str());
                ImGui::TableSetColumnIndex(0);
                AppCell(dl, ImGui::GetCursorScreenPos(), rowH, r.package, app);
                ImGui::Dummy(ImVec2(0, rowH));
                const auto hist = m_history.find(r.package);
                // CPU: value + sparkline.
                ImGui::TableSetColumnIndex(1);
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    CellText(rowH, std::format("{:.1f}%", r.cpu), r.cpu > 15.0 ? Warning : Text);
                    if (hist != m_history.end())
                        Sparkline(dl, ImVec2(p.x + S(62), p.y + S(14)), ImVec2(S(95), rowH - S(28)), hist->second.cpu, maxCpu, r.cpu > 15.0 ? Warning : Accent);
                }
                ImGui::TableSetColumnIndex(2);
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const bool big = r.ramBytes > 500'000'000;
                    CellText(rowH, ui::Bytes(r.ramBytes), big ? Warning : Text);
                    if (hist != m_history.end())
                        Sparkline(dl, ImVec2(p.x + S(72), p.y + S(14)), ImVec2(S(88), rowH - S(28)), hist->second.ram, maxRam, big ? Warning : Info);
                }
                ImGui::TableSetColumnIndex(3);
                CellText(rowH, std::to_string(r.processes), TextDim);
                ImGui::TableSetColumnIndex(4);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetFrameHeight()) * 0.5f);
                if (ui::SecondaryButton("Force stop", ImVec2(S(110), 0), !system && ctx.ActiveConnected()))
                    ctx.requestForceStop = r.package;
                if (system)
                    ImGui::SetItemTooltip("System processes are left alone.");
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void BackgroundPage::DrawBatteryTab(AppContext& ctx, float)
{
    bool loaded, loading;
    std::string error;
    BatteryDrain drain;
    {
        std::lock_guard l(m_shared->mutex);
        loaded = m_shared->batteryLoaded;
        loading = m_shared->batteryLoading;
        error = m_shared->batteryError;
        drain = m_shared->drain;
    }
    if (!loaded && !loading)
        LoadBattery(ctx);
    // Hand the numbers to Apps / Ask AI once per load.
    const double fingerprint = drain.computedMah + static_cast<double>(drain.entries.size());
    if (loaded && drain.found && m_batteryPublished != fingerprint) {
        m_batteryPublished = fingerprint;
        ctx.usage.SetBattery(drain, ctx.apps.Apps());
    }

    ImGui::AlignTextToFramePadding();
    ui::ColoredText(Warning, ICON_INFO "  Estimate");
    ImGui::SameLine(0, S(8));
    ui::DimText(drain.found ? std::format("Since the last full charge: Android computes {:.0f} mAh used (battery {} mAh). Per-app numbers are rough.",
                                  drain.computedMah, drain.capacityMah)
                            : std::string("Battery use since the last full charge, from Android's batterystats."));
    ImGui::SameLine();
    const float refreshW = ui::TextWidth(ICON_ROTATE "  Refresh") + S(28);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - refreshW));
    if (ui::SecondaryButton(ICON_ROTATE "  Refresh", ImVec2(refreshW, 0), ctx.ActiveConnected() && !loading))
        LoadBattery(ctx);

    const float h0 = ImGui::GetContentRegionAvail().y;
    if (!loaded || loading) {
        ui::SkeletonRows(std::max(3, static_cast<int>(h0 / S(44))), S(44));
        return;
    }
    if (!error.empty()) {
        if (ui::ErrorCard("baterr", "No battery estimate available.", error))
            LoadBattery(ctx);
        return;
    }

    // Rows: apps merged across users (main + Secure Folder etc.), hardware rows as they are.
    struct Row {
        std::string name, sub;
        double mAh = 0;
        const AppInfo* app = nullptr;
        bool isApp = false;
    };
    std::map<int, const AppInfo*> byUid;
    for (const auto& a : ctx.apps.Apps())
        if (a.uid >= 0)
            byUid[a.uid % 100000] = &a;
    std::map<int, Row> apps;
    std::vector<Row> rows;
    for (const auto& e : drain.entries) {
        if (e.key == "Over-counted")
            continue; // Android's correction term, not real drain
        if (e.appId >= 10000) {
            auto& r = apps[e.appId];
            r.isApp = true;
            r.mAh += e.mAh;
            if (r.name.empty()) {
                auto it = byUid.find(e.appId);
                r.app = it != byUid.end() ? it->second : nullptr;
                r.name = r.app ? std::string(r.app->DisplayName()) : "App uid " + std::to_string(e.appId);
                r.sub = r.app && !r.app->label.empty() ? r.app->package : (r.app ? std::string() : e.key);
            }
            if (e.userId != 0 && r.sub.find("other profiles") == std::string::npos)
                r.sub += "  \xC2\xB7  incl. other profiles";
        } else {
            Row r;
            r.name = DrainLabel(e);
            r.mAh = e.mAh;
            rows.push_back(std::move(r));
        }
    }
    for (auto& [id, r] : apps)
        rows.push_back(std::move(r));
    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.mAh > b.mAh; });
    const double total = drain.computedMah > 0 ? drain.computedMah : 1.0;
    const double maxV = rows.empty() ? 1.0 : std::max(0.001, rows.front().mAh);

    const float rowH = S(44);
    ImGui::BeginChild("##battery", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Row& r = rows[i];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float lh = ImGui::GetTextLineHeight();
            const float nameW = w * 0.38f;
            {
                Font f(GetFonts().uiStrong, FontBase);
                dl->AddText(ImVec2(p.x + S(12), p.y + (r.sub.empty() ? (rowH - lh) * 0.5f : S(4))), U32(r.isApp ? Text : TextDim), r.name.c_str());
            }
            if (!r.sub.empty()) {
                Font f(GetFonts().mono, FontSmall);
                dl->AddText(ImVec2(p.x + S(12), p.y + S(4) + lh + S(1)), U32(TextDim), r.sub.c_str());
            }
            // Bar + numbers.
            const float bx = p.x + nameW, bw = w - nameW - S(180);
            const float frac = static_cast<float>(r.mAh / maxV);
            dl->AddRectFilled(ImVec2(bx, p.y + rowH * 0.5f - S(4)), ImVec2(bx + bw, p.y + rowH * 0.5f + S(4)), U32(Border, 0.5f), S(4));
            dl->AddRectFilled(ImVec2(bx, p.y + rowH * 0.5f - S(4)), ImVec2(bx + std::max(S(2), bw * frac), p.y + rowH * 0.5f + S(4)),
                U32(r.isApp ? Accent : Info, 0.85f), S(4));
            const std::string v = std::format("{:.1f} mAh \xC2\xB7 {:.1f}%", r.mAh, r.mAh / total * 100.0);
            dl->AddText(ImVec2(bx + bw + S(14), p.y + (rowH - lh) * 0.5f), U32(Text), v.c_str());
            ImGui::Dummy(ImVec2(w, rowH));
        }
    }
    ImGui::EndChild();
}

} // namespace ps
