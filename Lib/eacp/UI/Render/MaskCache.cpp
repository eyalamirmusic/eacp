#include "MaskCache.h"

namespace eacp::UI
{
namespace
{
bool samePoints(const GPUWidgets::Path::SubPath& a,
                const GPUWidgets::Path::SubPath& b)
{
    if (a.closed != b.closed || a.points.size() != b.points.size())
        return false;

    for (auto i = 0; i < a.points.size(); ++i)
        if (a.points[i].x != b.points[i].x || a.points[i].y != b.points[i].y)
            return false;

    return true;
}

// What the key claims, checked. Compared point by point rather than by hashing
// again, since the whole purpose of it is to be the thing the hash could be
// wrong about.
bool sameGeometry(const GPUWidgets::Path& a,
                  GPUWidgets::FillRule ruleA,
                  const GPUWidgets::Path& b,
                  GPUWidgets::FillRule ruleB)
{
    if (ruleA != ruleB)
        return false;

    const auto& first = a.getSubPaths();
    const auto& second = b.getSubPaths();

    if (first.size() != second.size())
        return false;

    for (auto i = 0; i < first.size(); ++i)
        if (!samePoints(first[i], second[i]))
            return false;

    return true;
}
} // namespace

void MaskCache::setScale(float newScale)
{
    if (scale == newScale)
        return;

    scale = newScale;
    clear();
}

void MaskCache::clear()
{
    entries.clear();
}

const MaskCache::Entry* MaskCache::take(std::uint64_t key,
                                        const GPUWidgets::Path& path,
                                        GPUWidgets::FillRule rule)
{
    auto found = entries.find(key);

    if (found == entries.end())
        return nullptr;

    auto& record = found->second;

    // A collision reads as a miss. The shape rasterizes its own mask and does
    // not publish over the entry that is already there, so the two coexist with
    // one of them shareable -- which is the cheap half of being wrong about a
    // key, and the other half never happens.
    if (!sameGeometry(record.path, record.rule, path, rule))
        return nullptr;

    record.taken = true;
    ++shared;

    return &record.entry;
}

void MaskCache::publish(std::uint64_t key,
                        const GPUWidgets::Path& path,
                        GPUWidgets::FillRule rule,
                        const Entry& entry)
{
    if (entries.find(key) != entries.end())
        return;

    auto record = Record {};
    record.path = path;
    record.rule = rule;
    record.entry = entry;

    entries.emplace(key, std::move(record));
}

bool MaskCache::reclaim(std::uint64_t key)
{
    auto found = entries.find(key);

    if (found == entries.end())
        return false;

    if (found->second.taken)
        return false;

    entries.erase(found);

    return true;
}
} // namespace eacp::UI
