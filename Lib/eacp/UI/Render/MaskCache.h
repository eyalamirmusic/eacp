#pragma once

#include "CoverageAtlas.h"

#include <cstdint>
#include <unordered_map>

namespace eacp::UI
{
// Rasterized coverage keyed by geometry, so two components drawing the same
// shape share one slot. A published slot may be rewritten by its publisher only
// while nobody has taken it. clear() must accompany atlas.forgetAllocations().
class MaskCache
{
public:
    struct Entry
    {
        CoverageAtlas::Slot slot;
        Rect maskUV;
        Rect bounds;
    };

    // Coverage is device pixels, so a scale change drops every entry.
    void setScale(float newScale);

    void clear();

    // Null when nothing matches. Taking one marks it shared for good: its
    // publisher may no longer rasterize over the slot.
    const Entry* take(std::uint64_t key,
                      const GPUWidgets::Path& path,
                      GPUWidgets::FillRule rule);

    // Ignored when the key is already spoken for.
    void publish(std::uint64_t key,
                 const GPUWidgets::Path& path,
                 GPUWidgets::FillRule rule,
                 const Entry& entry);

    // False when somebody took it, and then the slot belongs to them.
    bool reclaim(std::uint64_t key);

    int getEntryCount() const { return (int) entries.size(); }

    // Masks shared rather than rasterized, since the count was last cleared.
    int getSharedCount() const { return shared; }
    void clearSharedCount() { shared = 0; }

private:
    struct Record
    {
        // Kept so a key match can be confirmed rather than trusted.
        GPUWidgets::Path path;
        GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero;

        Entry entry;

        // Set once anybody but the publisher holds the slot, after which it is
        // immutable: holders have already recorded its uv.
        bool taken = false;
    };

    std::unordered_map<std::uint64_t, Record> entries;

    float scale = 0.f;
    int shared = 0;
};
} // namespace eacp::UI
