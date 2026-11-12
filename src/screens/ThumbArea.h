#pragma once
#include "thumbs/ThumbCache.h"

#include <imgui.h>

namespace ps {

struct FileEntry;

// Thumbnail (or category icon) inside [a, b]. fill=true crops to cover (grid tiles),
// false letterboxes (previews). Shimmers while loading; videos get a play badge.
void DrawThumbArea(ImDrawList* dl, ImVec2 a, ImVec2 b, const FileEntry& e, const ThumbView& v, bool fill, float rounding, float iconSize);

} // namespace ps
