#pragma once
#include "scan/FileModel.h"

#include <vector>

namespace ps {

// Deterministic, realistic-looking file listing for --fake runs (UI work without a phone).
std::vector<Volume> FakeVolumes();
std::vector<FileEntry> FakeFiles(size_t count);

} // namespace ps
