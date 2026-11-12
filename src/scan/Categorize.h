#pragma once
#include "scan/FileModel.h"

#include <optional>
#include <string_view>

namespace ps {

// MIME first ("image/jpeg" -> Images); nullopt when the MIME is missing or too generic.
std::optional<Category> CategoryFromMime(std::string_view mime);
// Extension of a file name, case-insensitive ("IMG.JPG" -> Images).
Category CategoryFromName(std::string_view fileName);
Category Categorize(std::string_view fileName, std::string_view mime);

// relPath is relative to the volume root, e.g. "DCIM/Camera/IMG_1.jpg".
Source SourceFromPath(std::string_view relPath);
Protection ProtectionFromPath(std::string_view relPath);

// Fills relOffset/nameOffset/category/source/protection from path + volume root.
void Classify(FileEntry& e, std::string_view volumeRoot);

} // namespace ps
