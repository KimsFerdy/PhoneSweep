#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ps {

struct AppContext;

// "Delete permanently" for hand-picked files (Files right-click, selection bar,
// preview window). Applies the Nuke Protector rule (DecideNuke): in Nuke prompt
// mode only files saved + verified on the PC go; KEEP files only once saved;
// protected folders only in Nuke free (an explicit pick counts as "ticked").
// Draw() must be called from the same window/popup that called Ask().
class FileDeleter {
public:
    void Ask(AppContext& ctx, const std::vector<uint32_t>& indices);
    void Draw(AppContext& ctx);

private:
    bool m_open = false;
    std::vector<std::string> m_paths; // allowed
    uint64_t m_bytes = 0;
    std::vector<uint32_t> m_unsaved;  // blocked because not saved (can be saved first)
    size_t m_protected = 0;           // blocked: protected folder (Nuke prompt)
};

} // namespace ps
