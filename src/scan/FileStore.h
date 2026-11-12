#pragma once
#include "scan/FileModel.h"
#include "scan/ScanParse.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ps {

struct Totals {
    uint64_t count = 0;
    uint64_t bytes = 0;
    void Add(uint64_t size) { ++count; bytes += size; }
};

// All files of the active device's last scan, plus user selection/keep state.
// Owned and touched by the UI thread only; the scan thread hands over batches.
class FileStore {
public:
    void Clear();
    void SetVolumes(std::vector<Volume> v) { m_volumes = std::move(v); ++m_version; }
    void Append(std::vector<FileEntry>&& batch);
    // Applies MediaStore MIME types and re-categorizes affected files.
    size_t ApplyMime(const std::vector<MediaRow>& rows);
    // Drops files that were deleted from the phone. Indices change; selection is cleared.
    size_t RemovePaths(const std::vector<std::string>& paths);

    const std::vector<FileEntry>& Files() const { return m_files; }
    const std::vector<Volume>& Volumes() const { return m_volumes; }
    // Bumps whenever files/categories/keep flags change; views cache on it.
    uint64_t Version() const { return m_version; }

    // Totals exclude protected files (they're shown separately).
    const Totals& All() const { return m_all; }
    const Totals& ByCategory(Category c) const { return m_byCategory[static_cast<size_t>(c)]; }
    const Totals& BySource(Source s) const { return m_bySource[static_cast<size_t>(s)]; }
    const Totals& Protected() const { return m_protected; }

    // Selection (transient, for bulk actions).
    bool IsSelected(size_t i) const { return i < m_selected.size() && m_selected[i]; }
    void SetSelected(size_t i, bool on);
    void ClearSelection();
    const Totals& Selection() const { return m_selection; }

    // Keep flag (persistent intent: "save this before nuking").
    void SetKeep(size_t i, bool keep);
    const Totals& Kept() const { return m_kept; }

private:
    void AddToTotals(const FileEntry& e);
    void RecomputeTotals();

    std::vector<Volume> m_volumes;
    std::vector<FileEntry> m_files;
    std::vector<uint8_t> m_selected;
    uint64_t m_version = 0;
    Totals m_all, m_protected, m_selection, m_kept;
    std::array<Totals, kCategoryCount> m_byCategory{};
    std::array<Totals, kSourceCount> m_bySource{};
};

} // namespace ps
