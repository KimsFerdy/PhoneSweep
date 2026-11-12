#include "screens/FilesPage.h"
#include "screens/ThumbArea.h"

#include "app/AppContext.h"
#include "platform/Platform.h"
#include "scan/FileStore.h"
#include "scan/ScanSession.h"
#include "thumbs/ThumbCache.h"
#include "transfer/Manifest.h"
#include "transfer/Puller.h"
#include "util/Utf8.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>

namespace ps {

using namespace ps::theme;

namespace {

struct SizeFilter {
    const char* label;
    uint64_t min;
};
constexpr SizeFilter kSizeFilters[] = {
    { "Any size", 0 },
    { "> 1 MB", 1'000'000 },
    { "> 10 MB", 10'000'000 },
    { "> 50 MB", 50'000'000 },
    { "> 100 MB", 100'000'000 },
    { "> 1 GB", 1'000'000'000 },
};

struct DateFilter {
    const char* label;
    int64_t newerThanDays; // >0: modified within N days
    int64_t olderThanDays; // >0: modified more than N days ago
};
constexpr DateFilter kDateFilters[] = {
    { "Any date", 0, 0 },
    { "Last 7 days", 7, 0 },
    { "Last 30 days", 30, 0 },
    { "Last 12 months", 365, 0 },
    { "Older than 1 year", 0, 365 },
    { "Older than 2 years", 0, 730 },
};

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool IContains(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > hay.size())
        return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && Lower(hay[i + j]) == Lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

int ICompare(std::string_view a, std::string_view b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const char x = Lower(a[i]), y = Lower(b[i]);
        if (x != y)
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

// Shortens UTF-8 text to fit `width`, adding an ellipsis.
std::string FitText(std::string_view s, float width)
{
    if (ImGui::CalcTextSize(s.data(), s.data() + s.size()).x <= width)
        return std::string(s);
    const char* ell = "\xE2\x80\xA6";
    const float ellW = ImGui::CalcTextSize(ell).x;
    size_t end = s.size();
    while (end > 0) {
        do
            --end;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80);
        if (ImGui::CalcTextSize(s.data(), s.data() + end).x + ellW <= width)
            break;
    }
    return std::string(s.substr(0, end)) + ell;
}

std::string FolderLabel(const FileStore& store, const FileEntry& e)
{
    std::string folder(e.RelFolder());
    if (folder.empty())
        folder = "/";
    if (e.volume > 0 && e.volume < store.Volumes().size())
        return store.Volumes()[e.volume].label + " \xC2\xB7 " + folder;
    return folder;
}

void DrawCheckBox(ImDrawList* dl, ImVec2 p, float size, bool on)
{
    if (on) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), U32(Accent), S(3));
        const float t = size;
        dl->AddPolyline(std::array{ ImVec2(p.x + t * 0.22f, p.y + t * 0.52f), ImVec2(p.x + t * 0.42f, p.y + t * 0.72f),
                            ImVec2(p.x + t * 0.78f, p.y + t * 0.3f) }.data(),
            3, U32(Background), 0, std::max(1.5f, t * 0.14f));
    } else {
        dl->AddRect(p, ImVec2(p.x + size, p.y + size), U32(TextDim, 0.7f), S(3), 0, 1.2f);
    }
}

} // namespace

// Thumbnail (or icon) inside [a, b]. fill=true crops to cover (grid), false letterboxes (preview).
void DrawThumbArea(ImDrawList* dl, ImVec2 a, ImVec2 b, const FileEntry& e, const ThumbView& v, bool fill, float rounding,
    float iconSize)
{
    const ImVec4 cat = ui::CategoryColor(static_cast<uint8_t>(e.category));
    dl->AddRectFilled(a, b, U32(cat, 0.08f), rounding);
    const float bw = b.x - a.x, bh = b.y - a.y;

    if (v.state == ThumbState::Ready && v.texture && v.w > 0 && v.h > 0) {
        const float imgAspect = static_cast<float>(v.w) / static_cast<float>(v.h);
        const float boxAspect = bw / bh;
        ImVec2 uv0(0, 0), uv1(1, 1), pa = a, pb = b;
        if (fill) {
            // Crop the longer side so the image covers the box.
            if (imgAspect > boxAspect) {
                const float u = boxAspect / imgAspect;
                uv0.x = (1 - u) * 0.5f;
                uv1.x = 1 - uv0.x;
            } else {
                const float vv = imgAspect / boxAspect;
                uv0.y = (1 - vv) * 0.5f;
                uv1.y = 1 - uv0.y;
            }
        } else {
            // Fit inside, centred.
            float w = bw, h = bw / imgAspect;
            if (h > bh) {
                h = bh;
                w = bh * imgAspect;
            }
            pa = ImVec2(a.x + (bw - w) * 0.5f, a.y + (bh - h) * 0.5f);
            pb = ImVec2(pa.x + w, pa.y + h);
        }
        dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(v.texture)), pa, pb, uv0, uv1, U32Raw(ImVec4(1, 1, 1, ImGui::GetStyle().Alpha)),
            rounding);
        if (e.category == Category::Videos) {
            // Play badge.
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float r = std::min(bw, bh) * 0.14f;
            dl->AddCircleFilled(c, r, U32Raw(Hex(0x000000), 0.55f));
            dl->AddTriangleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.5f), ImVec2(c.x - r * 0.35f, c.y + r * 0.5f),
                ImVec2(c.x + r * 0.55f, c.y), U32(Text));
        }
        return;
    }

    {
        Font f(GetFonts().ui, iconSize);
        const char* icon = e.IsProtected() ? ICON_LOCK : ui::CategoryIcon(static_cast<uint8_t>(e.category));
        const ImVec2 ts = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), U32(e.IsProtected() ? TextDim : cat, 0.85f), icon);
    }
    if (v.state == ThumbState::Loading) {
        // Soft shimmer while the thumbnail is on its way.
        const float t = static_cast<float>(ImGui::GetTime());
        const float k = 0.5f + 0.5f * std::sin(t * 3.5f + a.x * 0.01f + a.y * 0.013f);
        dl->AddRectFilled(a, b, U32(Text, 0.03f + 0.04f * k), rounding);
    }
}

namespace {

// File state chips (UI spec §6): KEEP, SAVED ✓, QUEUED, SAVING, FAILED.
// Drawn BEFORE the file name (each followed by SameLine) so long names get
// clipped instead of the chips.
void StateChips(const AppContext& ctx, const FileEntry& e)
{
    auto chip = [](const char* text, const ImVec4& col, const char* tip) {
        ui::Pill(text, col);
        if (tip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, S(6));
    };
    if (ctx.verified.IsVerified(e.path, e.size, e.mtime))
        chip("SAVED " ICON_CHECK, Success, "Copied to the PC and verified");
    else if (const auto st = ctx.saveQueue.StateOf(e.path)) {
        switch (*st) {
        case SaveState::Queued: chip("QUEUED", TextDim, nullptr); break;
        case SaveState::Pulling:
        case SaveState::Verifying: chip("SAVING", Info, nullptr); break;
        case SaveState::Failed: chip("FAILED", Danger, "See Save queue for the reason"); break;
        default: break; // Done/Skipped show as SAVED once the index catches up
        }
    }
    if (e.flags & FileFlag_Keep)
        chip("KEEP", Accent, "Marked to keep: save it before anything is deleted");
}

} // namespace

// ---------------------------------------------------------------------------

void FilesPage::Draw(AppContext& ctx)
{
    FileStore& store = ctx.files;
    HandleFocusRequest(ctx);

    // Rebuild the filtered/sorted view when inputs change; throttle during a scan.
    const double now = ImGui::GetTime();
    if (m_viewDirty || (store.Version() != m_viewVersion && (!ctx.scan.Running() || now - m_lastRebuild > 0.4))) {
        RebuildView(store);
        m_lastRebuild = now;
    }

    HandleKeys(ctx);

    const float drawerW = m_preview >= 0 ? S(340) : 0.0f;
    const float selBarH = store.Selection().count > 0 ? S(54) : 0.0f;
    const float h = ImGui::GetContentRegionAvail().y;

    DrawSidebar(ctx, h);
    ImGui::SameLine(0, S(16));

    ImGui::BeginGroup();
    const float mainW = ImGui::GetContentRegionAvail().x - (drawerW > 0 ? drawerW + S(12) : 0.0f);
    ImGui::BeginChild("##filesmain", ImVec2(mainW, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    DrawToolbar(ctx);
    ImGui::Dummy(ImVec2(0, S(4)));
    const float listH = ImGui::GetContentRegionAvail().y - selBarH;

    const auto state = ctx.scan.GetState();
    if (store.Files().empty() && state != ScanSession::State::Running) {
        DrawStatusArea(ctx, listH);
    } else if (store.Files().empty()) {
        ui::SkeletonRows(std::max(3, static_cast<int>(listH / S(34))), S(34));
    } else if (m_view.empty()) {
        ImGui::BeginChild("##empty", ImVec2(0, listH));
        ui::EmptyState(ICON_SEARCH, "No files match", "Try clearing the search or filters.");
        ImGui::EndChild();
    } else if (m_grid) {
        DrawGrid(ctx, listH);
    } else {
        DrawList(ctx, listH);
    }
    if (selBarH > 0)
        DrawSelectionBar(ctx);
    ImGui::EndChild();
    ImGui::EndGroup();

    if (drawerW > 0) {
        ImGui::SameLine(0, S(12));
        DrawDrawer(ctx, h);
    }
}

void FilesPage::DrawStatusArea(AppContext& ctx, float height)
{
    ImGui::BeginChild("##status", ImVec2(0, height));
    const auto state = ctx.scan.GetState();
    if (state == ScanSession::State::Failed) {
        ImGui::Dummy(ImVec2(0, S(24)));
        const auto o = ctx.scan.Outcome();
        if (ui::ErrorCard("scanerr", o.error, o.errorDetail, ICON_ROTATE "  Retry scan"))
            ctx.requestScan = true;
    } else {
        ui::EmptyState(ICON_FILES, state == ScanSession::State::Cancelled ? "Scan cancelled" : "No scan yet",
            "Scan the phone to list its files.");
        ImGui::Dummy(ImVec2(0, S(10)));
        const char* label = ICON_ROTATE "  Scan now";
        const float bw = ui::TextWidth(label) + S(28) + S(4);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f);
        if (ui::PrimaryButton(label, ImVec2(bw, 0)))
            ctx.requestScan = true;
    }
    ImGui::EndChild();
}

// ---- Sidebar ----------------------------------------------------------------

void FilesPage::DrawSidebar(AppContext& ctx, float height)
{
    const FileStore& store = ctx.files;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(2)));
    ImGui::BeginChild("##filecats", ImVec2(S(220), height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = S(30);

    auto row = [&](const char* id, const ImVec4* dot, const char* label, const Totals& t, bool active, bool dim) -> bool {
        ImGui::PushID(id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool clicked = ImGui::InvisibleButton("##r", ImVec2(w, rowH));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (active || hovered)
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), U32(active ? PanelRaised : PanelHover, active ? 1.0f : 0.6f), S(RoundButton));
        if (active)
            dl->AddRectFilled(ImVec2(p.x, p.y + S(7)), ImVec2(p.x + S(3), p.y + rowH - S(7)), U32(Accent), S(2));
        const float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        float x = p.x + S(10);
        if (dot) {
            dl->AddCircleFilled(ImVec2(x + S(4), p.y + rowH * 0.5f), S(4), U32(*dot));
            x += S(16);
        }
        // Right-aligned count; the label is clipped so long names never run into it.
        const std::string count = ui::Count(t.count);
        const float cw = ui::TextWidth(count);
        dl->PushClipRect(ImVec2(x, p.y), ImVec2(p.x + w - cw - S(14), p.y + rowH), true);
        dl->AddText(ImVec2(x, ty), U32(dim ? TextDim : active ? Text : Mix(Text, TextDim, 0.25f)), label);
        dl->PopClipRect();
        dl->AddText(ImVec2(p.x + w - cw - S(8), ty), U32(TextDim), count.c_str());
        if (hovered && t.count)
            ImGui::SetTooltip("%s files \xC2\xB7 %s", ui::Count(t.count).c_str(), ui::Bytes(t.bytes).c_str());
        ImGui::PopID();
        return clicked;
    };

    auto header = [&](const char* text) {
        Font f(GetFonts().uiStrong, FontSmall);
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(10));
        ui::DimText(text);
        ImGui::Dummy(ImVec2(0, S(2)));
    };

    header("CATEGORIES");
    if (row("all", nullptr, "All", store.All(), m_category < 0, false)) {
        m_category = -1;
        Invalidate();
    }
    for (size_t c = 0; c < kCategoryCount; ++c) {
        const ImVec4 col = ui::CategoryColor(static_cast<uint8_t>(c));
        const Totals& t = store.ByCategory(static_cast<Category>(c));
        if (row(CategoryName(static_cast<Category>(c)), &col, CategoryName(static_cast<Category>(c)), t,
                m_category == static_cast<int>(c), t.count == 0)) {
            m_category = m_category == static_cast<int>(c) ? -1 : static_cast<int>(c);
            Invalidate();
        }
    }

    // Size of the current category, under the list.
    {
        Font f(GetFonts().ui, FontSmall);
        const Totals& t = m_category < 0 ? store.All() : store.ByCategory(static_cast<Category>(m_category));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(10));
        ui::DimText(std::format("{} in {}", ui::Bytes(t.bytes), m_category < 0 ? "all files" : CategoryName(static_cast<Category>(m_category))));
    }

    ImGui::Dummy(ImVec2(0, S(10)));
    header("SOURCES");
    for (size_t s = 0; s < kSourceCount; ++s) {
        const Totals& t = store.BySource(static_cast<Source>(s));
        if (t.count == 0 && static_cast<Source>(s) != Source::Other)
            continue;
        const bool messaging = s == static_cast<size_t>(Source::WhatsApp) || s == static_cast<size_t>(Source::WhatsAppBusiness)
            || s == static_cast<size_t>(Source::Telegram);
        const ImVec4 dotCol = messaging ? Success : TextDim;
        if (row(SourceName(static_cast<Source>(s)), messaging ? &dotCol : nullptr, SourceName(static_cast<Source>(s)), t,
                m_source == static_cast<int>(s), false)) {
            m_source = m_source == static_cast<int>(s) ? -1 : static_cast<int>(s);
            Invalidate();
        }
    }

    ImGui::Dummy(ImVec2(0, S(10)));
    ui::HLine(0.8f);
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), S(4)));
    if (ImGui::Checkbox("Show protected", &m_showProtected))
        Invalidate();
    ImGui::PopStyleVar();
    ImGui::SameLine(0, S(4));
    ui::ColoredText(m_showProtected ? Warning : TextDim, ICON_LOCK);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Android/data, Android/obb and hidden (dot) folders hold app data.\n"
                          "They're shown greyed out and Nuke never touches them\nunless you unlock them explicitly.");
    {
        Font f(GetFonts().ui, FontSmall);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(4));
        ui::DimText(std::format("{} files \xC2\xB7 {}", ui::Count(store.Protected().count), ui::Bytes(store.Protected().bytes)));
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- Toolbar ----------------------------------------------------------------

void FilesPage::DrawToolbar(AppContext& ctx)
{
    const FileStore& store = ctx.files;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(7)));

    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    // Layout: one row when it fits, else search + view toggle on top and filters below.
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float segW = S(36);
    const float togglesW = segW * 2 + S(2);
    const float comboW[3] = { S(110), S(150), S(150) };
    const float combosW = comboW[0] + comboW[1] + comboW[2] + sp * 3;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float minSearch = S(220);
    const bool oneRow = avail >= minSearch + combosW + togglesW + sp;
    const float searchW = oneRow ? std::clamp(avail * 0.3f, minSearch, avail - combosW - togglesW - sp * 2) : avail - togglesW - sp;

    ImGui::SetNextItemWidth(searchW);
    if (ImGui::InputTextWithHint("##search", ICON_SEARCH "  Search name or folder  (Ctrl+F)", m_search, sizeof(m_search)))
        Invalidate();

    bool firstOnRow = false;
    auto combo = [&](const char* id, int& value, auto&& labels, int count, float width) {
        if (!firstOnRow)
            ImGui::SameLine();
        firstOnRow = false;
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo(id, labels(value), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(labels(i), value == i)) {
                    value = i;
                    Invalidate();
                }
            ImGui::EndCombo();
        }
    };
    auto combos = [&] {
        combo("##size", m_sizeFilter, [](int i) { return kSizeFilters[i].label; }, static_cast<int>(std::size(kSizeFilters)), comboW[0]);
        combo("##date", m_dateFilter, [](int i) { return kDateFilters[i].label; }, static_cast<int>(std::size(kDateFilters)), comboW[1]);
        int src = m_source + 1;
        combo("##src", src, [](int i) { return i == 0 ? "Any source" : SourceName(static_cast<Source>(i - 1)); },
            static_cast<int>(kSourceCount) + 1, comboW[2]);
        if (src - 1 != m_source) {
            m_source = src - 1;
            Invalidate();
        }
    };
    if (oneRow)
        combos();

    // Grid | List toggle, right-aligned.
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - togglesW));
    auto seg = [&](const char* icon, bool active, const char* tip) {
        ImGui::PushStyleColor(ImGuiCol_Button, active ? Hex(0x3DD6C6, 0.18f) : PanelRaised);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Accent : TextDim);
        const bool pressed = ImGui::Button(icon, ImVec2(segW, 0));
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", tip);
        return pressed;
    };
    if (seg(ICON_GRID "##grid", m_grid, "Grid view"))
        m_grid = true;
    ImGui::SameLine(0, S(2));
    if (seg(ICON_BARS "##list", !m_grid, "List view"))
        m_grid = false;

    if (!oneRow) {
        firstOnRow = true;
        combos();
    }
    ImGui::PopStyleVar();

    // Result line.
    Font f(GetFonts().ui, FontSmall);
    std::string line = std::format("{} files \xC2\xB7 {}", ui::Count(m_view.size()), ui::Bytes(m_viewBytes));
    if (ctx.scan.Running())
        line += std::format("   (scanning\xE2\x80\xA6 {} found)", ui::Count(ctx.scan.FilesFound()));
    ui::DimText(line);
    (void)store;
}

// ---- View building -----------------------------------------------------------

void FilesPage::RebuildView(const FileStore& store)
{
    const auto& files = store.Files();
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto& df = kDateFilters[m_dateFilter];
    const uint64_t minSize = kSizeFilters[m_sizeFilter].min;
    const std::string_view search = m_search;

    m_view.clear();
    m_viewBytes = 0;
    for (uint32_t i = 0; i < files.size(); ++i) {
        const auto& e = files[i];
        if (!m_showProtected && e.IsProtected())
            continue;
        if (m_category >= 0 && static_cast<int>(e.category) != m_category)
            continue;
        if (m_source >= 0 && static_cast<int>(e.source) != m_source)
            continue;
        if (e.size < minSize)
            continue;
        if (df.newerThanDays > 0 && e.mtime < now - df.newerThanDays * 86400)
            continue;
        if (df.olderThanDays > 0 && e.mtime > now - df.olderThanDays * 86400)
            continue;
        if (!search.empty() && !IContains(e.RelPath(), search))
            continue;
        m_view.push_back(i);
        m_viewBytes += e.size;
    }
    Sort(store);
    m_viewVersion = store.Version();
    m_viewDirty = false;
    m_anchorRow = m_focusRow = -1;
}

void FilesPage::Sort(const FileStore& store)
{
    const auto& f = store.Files();
    const bool asc = m_sortAscending;
    auto cmp = [&](uint32_t a, uint32_t b) {
        const auto& x = f[a];
        const auto& y = f[b];
        int c = 0;
        switch (m_sortColumn) {
        case ColName: c = ICompare(x.Name(), y.Name()); break;
        case ColFolder: c = ICompare(x.RelFolder(), y.RelFolder()); break;
        case ColSource: c = static_cast<int>(x.source) - static_cast<int>(y.source); break;
        case ColDate: c = x.mtime < y.mtime ? -1 : x.mtime > y.mtime ? 1 : 0; break;
        default: c = x.size < y.size ? -1 : x.size > y.size ? 1 : 0; break;
        }
        if (c == 0)
            return a < b; // stable, deterministic
        return asc ? c < 0 : c > 0;
    };
    std::sort(m_view.begin(), m_view.end(), cmp);
}

void FilesPage::ClickRow(FileStore& store, int row, bool shift)
{
    if (row < 0 || row >= static_cast<int>(m_view.size()))
        return;
    if (shift && m_anchorRow >= 0 && m_anchorRow < static_cast<int>(m_view.size())) {
        const bool on = store.IsSelected(m_view[m_anchorRow]);
        const int lo = std::min(m_anchorRow, row), hi = std::max(m_anchorRow, row);
        for (int r = lo; r <= hi; ++r)
            store.SetSelected(m_view[r], on);
    } else {
        store.SetSelected(m_view[row], !store.IsSelected(m_view[row]));
        m_anchorRow = row;
    }
    m_focusRow = row;
}

void FilesPage::HandleKeys(AppContext& ctx)
{
    FileStore& store = ctx.files;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        m_focusSearch = true;
    if (ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        for (uint32_t i : m_view)
            store.SetSelected(i, true);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (m_preview >= 0)
            m_preview = -1;
        else
            store.ClearSelection();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && m_focusRow >= 0 && m_focusRow < static_cast<int>(m_view.size()))
        store.SetSelected(m_view[m_focusRow], !store.IsSelected(m_view[m_focusRow]));
}

void FilesPage::HandleFocusRequest(AppContext& ctx)
{
    // Dev preset (--files=...), applied once the scan has produced files.
    if (!ctx.filesPreset.empty() && !ctx.files.Files().empty() && !ctx.scan.Running()) {
        const std::string p = std::exchange(ctx.filesPreset, {});
        auto has = [&](std::string_view w) { return p.find(w) != std::string::npos; };
        m_grid = has("grid");
        if (has("images"))
            m_category = static_cast<int>(Category::Images);
        if (has("videos"))
            m_category = static_cast<int>(Category::Videos);
        if (has("camera"))
            m_source = static_cast<int>(Source::Camera);
        if (has("whatsapp"))
            m_source = static_cast<int>(Source::WhatsApp);
        RebuildView(ctx.files);
        if (has("preview") && !m_view.empty())
            m_preview = m_view.front();
        if (has("save")) { // queue the first 6 files of the view, then show the queue
            for (size_t i = 0; i < std::min<size_t>(6, m_view.size()); ++i)
                ctx.requestSaveFiles.push_back(m_view[i]);
            ctx.page = Page::SaveQueue;
        }
    }

    if (ctx.focusFile < 0)
        return;
    const auto idx = static_cast<uint32_t>(ctx.focusFile);
    ctx.focusFile = -1;
    if (idx >= ctx.files.Files().size())
        return;
    // Clear anything that could hide the file, then reveal + preview it.
    m_category = m_source = -1;
    m_sizeFilter = m_dateFilter = 0;
    m_search[0] = 0;
    m_grid = false;
    if (ctx.files.Files()[idx].IsProtected())
        m_showProtected = true;
    RebuildView(ctx.files);
    const auto it = std::ranges::find(m_view, idx);
    if (it != m_view.end()) {
        m_scrollToRow = static_cast<int>(it - m_view.begin());
        m_focusRow = m_anchorRow = m_scrollToRow;
    }
    m_preview = idx;
}

// ---- List view ----------------------------------------------------------------

void FilesPage::DrawList(AppContext& ctx, float height)
{
    FileStore& store = ctx.files;
    const auto& files = store.Files();
    const float rowH = ImGui::GetFrameHeight() + S(4);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(8), 0));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp
        | ImGuiTableFlags_Reorderable;
    if (!ImGui::BeginTable("##files", 6, flags, ImVec2(0, height))) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize
            | ImGuiTableColumnFlags_NoReorder, S(26), ColSelect);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f, ColName);
    ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 2.4f, ColFolder);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, S(130), ColSource);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort
            | ImGuiTableColumnFlags_PreferSortDescending, S(90), ColSize);
    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, S(100), ColDate);

    // Header with a select-all-in-view checkbox in the first column.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowH);
    for (int c = 0; c < 6; ++c) {
        if (!ImGui::TableSetColumnIndex(c))
            continue;
        if (c == ColSelect) {
            bool all = !m_view.empty();
            for (uint32_t i : m_view)
                if (!store.IsSelected(i)) {
                    all = false;
                    break;
                }
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float box = S(15);
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (rowH - box) * 0.5f));
            if (ImGui::InvisibleButton("##selall", ImVec2(box, box))) {
                for (uint32_t i : m_view)
                    store.SetSelected(i, !all);
            }
            ImGui::SetItemTooltip("Select all in view (Ctrl+A)");
            DrawCheckBox(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), box, all);
        } else {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TableHeader(ImGui::TableGetColumnName(c));
        }
    }

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) {
            m_sortColumn = static_cast<int>(specs->Specs[0].ColumnUserID);
            m_sortAscending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            Sort(store);
        }
        specs->SpecsDirty = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_view.size()), rowH);
    if (m_scrollToRow >= 0)
        clipper.IncludeItemByIndex(m_scrollToRow);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const uint32_t fi = m_view[row];
            const FileEntry& e = files[fi];
            const bool selected = store.IsSelected(fi);
            const bool prot = e.IsProtected();

            ImGui::TableNextRow(0, rowH);
            ImGui::PushID(static_cast<int>(fi));
            ImGui::TableSetColumnIndex(0);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", selected || m_preview == fi,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, rowH))) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_preview = fi;
                    // The first click of the double-click toggled it; undo that.
                    store.SetSelected(fi, !store.IsSelected(fi));
                } else {
                    ClickRow(store, row, ImGui::GetIO().KeyShift);
                }
            }
            if (row == m_scrollToRow) {
                ImGui::SetScrollHereY(0.4f);
                m_scrollToRow = -1;
            }
            if (prot && ImGui::IsItemHovered())
                ImGui::SetTooltip("Protected (%s): never touched by Nuke unless unlocked", ProtectionName(e.protection));
            DrawCheckBox(dl, ImVec2(rowMin.x, rowMin.y + (rowH - S(15)) * 0.5f), S(15), selected);

            auto cellText = [&](const std::string& s, const ImVec4& col) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
                ui::ColoredText(col, s);
            };
            const ImVec4 textCol = prot ? TextDim : Text;

            ImGui::TableSetColumnIndex(ColName);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
            ui::ColoredText(prot ? TextDim : ui::CategoryColor(static_cast<uint8_t>(e.category)),
                prot ? ICON_LOCK : ui::CategoryIcon(static_cast<uint8_t>(e.category)));
            ImGui::SameLine(0, S(8));
            StateChips(ctx, e); // chips first: the name is what gets clipped
            ui::ColoredText(textCol, e.Name());

            ImGui::TableSetColumnIndex(ColFolder);
            {
                Font f(GetFonts().ui, FontSmall);
                cellText(FolderLabel(store, e), TextDim);
            }
            ImGui::TableSetColumnIndex(ColSource);
            cellText(e.source == Source::Other ? std::string("\xE2\x80\x94") : SourceName(e.source), TextDim);
            ImGui::TableSetColumnIndex(ColSize);
            cellText(ui::Bytes(e.size), textCol);
            ImGui::TableSetColumnIndex(ColDate);
            cellText(ui::Date(e.mtime), TextDim);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

// ---- Grid view ----------------------------------------------------------------

void FilesPage::DrawGrid(AppContext& ctx, float height)
{
    FileStore& store = ctx.files;
    const auto& files = store.Files();
    ImGui::BeginChild("##grid", ImVec2(0, height));

    const float tileW = S(132), tileH = S(150), gap = S(12);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + gap) / (tileW + gap)));
    const int rows = (static_cast<int>(m_view.size()) + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGuiListClipper clipper;
    clipper.Begin(rows, tileH + gap);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int vi = r * cols + c;
                if (vi >= static_cast<int>(m_view.size()))
                    break;
                const uint32_t fi = m_view[vi];
                const FileEntry& e = files[fi];
                const bool selected = store.IsSelected(fi);
                if (c > 0)
                    ImGui::SameLine(0, gap);
                ImGui::PushID(static_cast<int>(fi));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##tile", ImVec2(tileW, tileH));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        m_preview = fi;
                        store.SetSelected(fi, !store.IsSelected(fi));
                    } else {
                        ClickRow(store, vi, ImGui::GetIO().KeyShift);
                    }
                }
                if (hovered)
                    ImGui::SetItemTooltip("%s\n%s \xC2\xB7 %s", std::string(e.Name()).c_str(), ui::Bytes(e.size).c_str(),
                        ui::Date(e.mtime).c_str());

                const ImVec2 q(p.x + tileW, p.y + tileH);
                const ImVec4 cat = ui::CategoryColor(static_cast<uint8_t>(e.category));
                dl->AddRectFilled(p, q, U32(hovered ? PanelHover : PanelRaised), S(RoundPanel));
                const ImVec2 tmin(p.x + S(6), p.y + S(6)), tmax(q.x - S(6), p.y + tileW - S(18));
                DrawThumbArea(dl, tmin, tmax, e, ctx.thumbs.Request(e, ThumbVariant::Grid), true, S(6), 30.0f);
                (void)cat;
                // Size badge bottom-right of the thumbnail.
                {
                    Font f(GetFonts().uiStrong, FontSmall - 2);
                    const std::string sz = ui::Bytes(e.size);
                    const ImVec2 ts = ImGui::CalcTextSize(sz.c_str());
                    const ImVec2 bmax(tmax.x - S(4), tmax.y - S(4));
                    const ImVec2 bmin(bmax.x - ts.x - S(10), bmax.y - ts.y - S(4));
                    dl->AddRectFilled(bmin, bmax, U32Raw(Hex(0x000000), 0.6f), S(4));
                    dl->AddText(ImVec2(bmin.x + S(5), bmin.y + S(2)), U32(Text), sz.c_str());
                }
                // Name (+ KEEP).
                {
                    Font f(GetFonts().ui, FontSmall);
                    const std::string name = FitText(e.Name(), tileW - S(12));
                    dl->AddText(ImVec2(p.x + S(6), tmax.y + S(6)), U32(e.IsProtected() ? TextDim : Text), name.c_str());
                    if (e.flags & FileFlag_Keep)
                        dl->AddText(ImVec2(p.x + S(6), tmax.y + S(6) + ImGui::GetTextLineHeight()), U32(Accent), "KEEP");
                }
                if (selected || hovered)
                    DrawCheckBox(dl, ImVec2(p.x + S(10), p.y + S(10)), S(16), selected);
                if (selected || m_preview == fi)
                    dl->AddRect(p, q, U32(Accent), S(RoundPanel), 0, 2.0f);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

// ---- Selection bar & drawer ---------------------------------------------------

void FilesPage::DrawSelectionBar(AppContext& ctx)
{
    FileStore& store = ctx.files;
    const Totals sel = store.Selection();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), 0));
    ImGui::BeginChild("##selbar", ImVec2(0, S(50)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    const float midY = S(50) * 0.5f;
    ImGui::SetCursorPosY(midY - ImGui::GetFrameHeight() * 0.5f);
    ImGui::AlignTextToFramePadding();
    {
        Font f(GetFonts().uiStrong, FontBase);
        ImGui::TextUnformatted(std::format("{} selected", ui::Count(sel.count)).c_str());
    }
    ImGui::SameLine();
    ui::DimText(std::format("\xC2\xB7 {}", ui::Bytes(sel.bytes)));

    const char* clearLabel = "Clear";
    const char* keepLabel = ICON_CHECK "  Mark as keep";
    const char* unkeepLabel = "Unmark";
    const char* saveLabel = ICON_FLOPPY "  Save selected";
    const float pad = S(28);
    const float total = ui::TextWidth(saveLabel) + ui::TextWidth(keepLabel) + ui::TextWidth(unkeepLabel) + ui::TextWidth(clearLabel)
        + pad * 4 + S(8) * 3 + S(4);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - total);
    ImGui::SetCursorPosY(midY - (ImGui::GetFrameHeight() + S(4)) * 0.5f);
    if (ui::PrimaryButton(saveLabel)) {
        const auto& files = store.Files();
        for (uint32_t i = 0; i < files.size(); ++i)
            if (store.IsSelected(i))
                ctx.requestSaveFiles.push_back(i);
    }
    ImGui::SetItemTooltip("Copies the selected files to the PC and verifies each one.");
    ImGui::SameLine(0, S(8));
    auto applyKeep = [&](bool keep) {
        size_t n = 0;
        const auto& files = store.Files();
        for (size_t i = 0; i < files.size(); ++i)
            if (store.IsSelected(i)) {
                store.SetKeep(i, keep);
                ++n;
            }
        ui::Toast(keep ? std::format("Marked {} files as keep", ui::Count(n)) : std::format("Unmarked {} files", ui::Count(n)));
    };
    if (ui::SecondaryButton(keepLabel))
        applyKeep(true);
    ImGui::SetItemTooltip("Keep = save to PC before anything gets deleted.");
    ImGui::SameLine(0, S(8));
    if (ui::SecondaryButton(unkeepLabel))
        applyKeep(false);
    ImGui::SameLine(0, S(8));
    if (ui::GhostButton(clearLabel))
        store.ClearSelection();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void FilesPage::DrawDrawer(AppContext& ctx, float height)
{
    FileStore& store = ctx.files;
    if (m_preview < 0 || static_cast<size_t>(m_preview) >= store.Files().size()) {
        m_preview = -1;
        return;
    }
    const FileEntry& e = store.Files()[static_cast<size_t>(m_preview)];

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild("##drawer", ImVec2(S(340), height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    // Close button.
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(28));
    if (ui::GhostButton(ICON_XMARK "##closedrawer"))
        m_preview = -1;

    // Preview: sharp variant when ready, the grid thumbnail meanwhile, else the icon.
    const float w = ImGui::GetContentRegionAvail().x;
    const float ph = w * 0.75f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ThumbView view = ctx.thumbs.Request(e, ThumbVariant::Preview);
    if (view.state != ThumbState::Ready) {
        if (const ThumbView grid = ctx.thumbs.Peek(e, ThumbVariant::Grid); grid.state == ThumbState::Ready)
            view = grid;
    }
    DrawThumbArea(ImGui::GetWindowDrawList(), p, ImVec2(p.x + w, p.y + ph), e, view, false, S(RoundPanel), 48.0f);
    ImGui::Dummy(ImVec2(w, ph));
    if (view.state == ThumbState::Unsupported) {
        Font f(GetFonts().ui, FontSmall);
        ui::DimText(e.category == Category::Videos ? "No preview: install ffmpeg for video thumbnails." : "No preview for this format.");
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    {
        Font f(GetFonts().uiStrong, FontBase + 1);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(e.Name().data(), e.Name().data() + e.Name().size());
        ImGui::PopTextWrapPos();
    }
    if (e.flags & FileFlag_Keep)
        ui::Pill("KEEP", Accent);
    if (e.IsProtected()) {
        ImGui::SameLine();
        ui::Pill(std::format("PROTECTED \xC2\xB7 {}", ProtectionName(e.protection)), Warning);
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    auto meta = [&](const char* label, const std::string& value) {
        ui::DimText(label);
        ImGui::SameLine(S(96));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    };
    meta("Size", std::format("{}  ({} bytes)", ui::Bytes(e.size), ui::Count(e.size)));
    meta("Modified", ui::Date(e.mtime));
    meta("Type", e.mime.empty() ? std::format("{} (by extension)", CategoryName(e.category)) : e.mime);
    meta("Source", SourceName(e.source));
    if (e.volume < store.Volumes().size())
        meta("Storage", store.Volumes()[e.volume].label);

    ImGui::Dummy(ImVec2(0, S(8)));
    ui::DimText("Full path");
    {
        Font f(GetFonts().mono, FontSmall);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(e.path.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ui::GhostButton(ICON_COPY "  Copy path")) {
        ctx.platform.SetClipboardText(e.path);
        ui::Toast("Copied to clipboard");
    }

    ImGui::Dummy(ImVec2(0, S(10)));
    const bool keep = e.flags & FileFlag_Keep;
    if (keep ? ui::SecondaryButton("Unmark keep") : ui::PrimaryButton(ICON_CHECK "  Keep"))
        store.SetKeep(static_cast<size_t>(m_preview), !keep);
    ImGui::SameLine();
    if (const auto local = ctx.verified.IsVerified(e.path, e.size, e.mtime) ? ctx.verified.LocalFile(e.path) : std::nullopt) {
        if (ui::SecondaryButton(ICON_FOLDER_OPEN "  Open folder on PC"))
            ctx.platform.OpenInExplorer(local->parent_path());
        ImGui::SetItemTooltip("%s", Narrow(local->wstring()).c_str());
    } else if (ui::SecondaryButton(ICON_FLOPPY "  Save")) {
        ctx.requestSaveFiles.push_back(static_cast<uint32_t>(m_preview));
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace ps
