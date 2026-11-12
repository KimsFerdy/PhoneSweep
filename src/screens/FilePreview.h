#pragma once
#include "screens/FileDeleter.h"
#include "screens/PreviewKit.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ps {

struct AppContext;

// Files → right-click → Preview (or Space): one file, big. Photos zoom (wheel) and
// pan (drag), videos open on the PC, music plays here, text is rendered. ← / → walk
// through the Files page's current (filtered, sorted) list.
class FilePreview {
public:
    void Open(const std::string& path);
    bool IsOpen() const { return !m_path.empty(); }
    void Close();

    // view: FileStore indices in display order (for prev / next).
    void Draw(AppContext& ctx, const std::vector<uint32_t>& view);

    PreviewKit& Kit() { return m_kit; } // also used by the Files right-click "Open on PC"

private:
    std::string m_path; // identity survives FileStore index changes (rescans, deletes)
    size_t m_pos = 0;   // last known position in the view
    bool m_justOpened = false;
    PreviewKit m_kit;
    FileDeleter m_deleter;
};

} // namespace ps
