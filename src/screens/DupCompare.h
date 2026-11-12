#pragma once
#include "cleanup/Cleanup.h"
#include "screens/PreviewKit.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

struct AppContext;
struct FileEntry;

// v1.1: one duplicate group, every copy side by side (image / video frame / audio
// player / text), each with its own Keep / Delete choice, and "Delete now" that
// removes this group's marked copies right away (after a confirmation).
class DupCompare {
public:
    using Lookup = std::function<const FileEntry*(std::string_view path)>;

    DupCompare();
    ~DupCompare();

    void Open(int group);
    bool IsOpen() const { return m_group >= 0; }
    void Close();
    void Draw(AppContext& ctx, std::vector<DupGroup>& groups, std::vector<bool>& ticked, const Lookup& lookup);
    // Deletes the group's marked copies now (what the confirmation's Delete does; also the dev self-test).
    void SendDelete(AppContext& ctx, const DupGroup& grp);

private:
    void DrawColumn(AppContext& ctx, DupGroup& grp, bool& ticked, size_t i, const FileEntry* e, float width, float height);
    void DrawConfirm(AppContext& ctx, DupGroup& grp);

    int m_group = -1;
    bool m_justOpened = false;
    bool m_confirm = false;              // open the "delete now?" popup
    std::vector<std::string> m_deleting; // sent to the phone: shown as "Deleting…" until they're gone
    int m_sentFrame = 0;
    uint64_t m_deletingBytes = 0;
    std::string m_notice; // "Deleted 1 copy · freed 2 MB", shown a few seconds
    double m_noticeUntil = 0;
    PreviewKit m_kit;
};

} // namespace ps
