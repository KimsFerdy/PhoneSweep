#include "scan/FileStore.h"

#include "scan/Categorize.h"

#include <string_view>
#include <unordered_map>

namespace ps {

void FileStore::Clear()
{
    m_files.clear();
    m_selected.clear();
    m_volumes.clear();
    m_all = m_protected = m_selection = m_kept = {};
    m_byCategory = {};
    m_bySource = {};
    ++m_version;
}

void FileStore::AddToTotals(const FileEntry& e)
{
    if (e.IsProtected()) {
        m_protected.Add(e.Disk());
        return;
    }
    m_all.Add(e.Disk());
    m_byCategory[static_cast<size_t>(e.category)].Add(e.Disk());
    m_bySource[static_cast<size_t>(e.source)].Add(e.Disk());
}

void FileStore::RecomputeTotals()
{
    m_all = m_protected = {};
    m_byCategory = {};
    m_bySource = {};
    for (const auto& e : m_files)
        AddToTotals(e);
}

void FileStore::Append(std::vector<FileEntry>&& batch)
{
    m_files.reserve(m_files.size() + batch.size());
    for (auto& e : batch) {
        AddToTotals(e);
        m_files.push_back(std::move(e));
    }
    m_selected.resize(m_files.size(), 0);
    ++m_version;
}

size_t FileStore::ApplyMime(const std::vector<MediaRow>& rows)
{
    std::unordered_map<std::string_view, std::string_view> byPath;
    byPath.reserve(rows.size());
    for (const auto& r : rows)
        byPath.emplace(r.path, r.mime);

    size_t changed = 0;
    for (auto& e : m_files) {
        auto it = byPath.find(e.path);
        if (it == byPath.end())
            continue;
        e.mime = it->second;
        const Category c = Categorize(e.Name(), e.mime);
        if (c != e.category) {
            e.category = c;
            ++changed;
        }
    }
    RecomputeTotals();
    ++m_version;
    return changed;
}

size_t FileStore::RemovePaths(const std::vector<std::string>& paths)
{
    if (paths.empty())
        return 0;
    std::unordered_map<std::string_view, bool> gone;
    gone.reserve(paths.size());
    for (const auto& p : paths)
        gone.emplace(p, true);
    const size_t before = m_files.size();
    std::erase_if(m_files, [&](const FileEntry& e) { return gone.contains(e.path); });
    m_selected.assign(m_files.size(), 0);
    m_selection = {};
    m_kept = {};
    for (const auto& e : m_files)
        if (e.flags & FileFlag_Keep)
            m_kept.Add(e.size);
    RecomputeTotals();
    ++m_version;
    return before - m_files.size();
}

void FileStore::SetSelected(size_t i, bool on)
{
    if (i >= m_files.size() || static_cast<bool>(m_selected[i]) == on)
        return;
    m_selected[i] = on;
    if (on) {
        m_selection.Add(m_files[i].size);
    } else {
        --m_selection.count;
        m_selection.bytes -= m_files[i].size;
    }
}

void FileStore::ClearSelection()
{
    std::fill(m_selected.begin(), m_selected.end(), 0);
    m_selection = {};
}

void FileStore::SetKeep(size_t i, bool keep)
{
    if (i >= m_files.size())
        return;
    auto& e = m_files[i];
    const bool was = e.flags & FileFlag_Keep;
    if (was == keep)
        return;
    e.flags = static_cast<uint8_t>(keep ? (e.flags | FileFlag_Keep) : (e.flags & ~FileFlag_Keep));
    if (keep) {
        m_kept.Add(e.size);
    } else {
        --m_kept.count;
        m_kept.bytes -= e.size;
    }
    ++m_version;
}

} // namespace ps
