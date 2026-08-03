#pragma once

#include "CoverageAtlas.h"

#include <cstdint>
#include <unordered_map>

namespace eacp::UI
{
// Coverage already rasterized, keyed by the geometry that produced it, so that
// two components drawing the same shape hold one slot between them instead of
// two copies of the same texels. ComponentTree's channel strips draw one knob
// arc forty-eight times over; this is what makes that one arc.
//
// The key is the geometry and nothing the caller supplies -- see ContentHash --
// and a hit compares the stored path before it shares the slot, so a collision
// costs a comparison rather than drawing the wrong shape.
//
// What this is careful about is *mutation*, which is where sharing meets the
// thing PathShape already does: a shape keeps the slot it was given and
// rasterizes into it again whenever the new mask still fits, and that is what
// stops a knob being dragged consuming the atlas one frame at a time. Rewriting
// a slot somebody else is sampling would draw this shape's coverage through
// that shape's quad -- wrong, and silently so. Hence the rule:
//
//   a published slot may be rewritten by its publisher, and only while nobody
//   else has ever taken it.
//
// So `take` marks an entry shared for good, and `reclaim` succeeds only on an
// entry nobody took. A shape whose geometry churns reclaims its slot on the
// first change and stops publishing; a shape whose geometry is settled keeps
// its entry and everything drawing that shape shares it. The pathological case
// -- many identical shapes that all animate -- forfeits exactly one slot, once,
// rather than one per frame.
//
// Nothing is refcounted, because there would be nothing to do with the answer:
// the atlas is a shelf with no free list, so a slot given back is not a slot
// reclaimed. Entries therefore live until the whole cache is dropped, which is
// what has to happen anyway whenever the atlas relocates -- see clear(), and
// note that it and CoverageAtlas::forgetAllocations are the one pair that must
// be called together.
class MaskCache
{
public:
    // A rasterized mask, and everything a shape needs to draw it without
    // rasterizing anything itself.
    struct Entry
    {
        CoverageAtlas::Slot slot;
        Rect maskUV;
        Rect bounds;
    };

    // Coverage is computed in device pixels, so everything held here was
    // rasterized at one scale and a different one invalidates the lot. Told
    // rather than keyed by, so that a display change drops the entries instead
    // of leaving them to be missed forever.
    void setScale(float newScale);

    void clear();

    // The mask already rasterized for this geometry, or null. Taking one marks
    // it shared: its publisher may no longer rasterize over the slot.
    const Entry* take(std::uint64_t key,
                      const GPUWidgets::Path& path,
                      GPUWidgets::FillRule rule);

    // Offers a freshly rasterized mask for others to share. Ignored when the
    // key is already spoken for, which is either the publisher offering the
    // same thing twice or a collision -- and in both cases the entry already
    // there is the one to keep.
    void publish(std::uint64_t key,
                 const GPUWidgets::Path& path,
                 GPUWidgets::FillRule rule,
                 const Entry& entry);

    // Takes back an entry nobody else ever took, so its publisher may rasterize
    // over the slot again. False when somebody did take it, and then the slot
    // belongs to them and the publisher has to allocate another.
    bool reclaim(std::uint64_t key);

    int getEntryCount() const { return (int) entries.size(); }

    // Masks that were shared rather than rasterized, since the count was last
    // cleared -- which is the whole claim of this class, so it is worth being
    // able to read.
    int getSharedCount() const { return shared; }
    void clearSharedCount() { shared = 0; }

private:
    struct Record
    {
        // Kept so that a key match can be confirmed rather than trusted. A key
        // is a summary; two paths that collide on one are still two shapes.
        GPUWidgets::Path path;
        GPUWidgets::FillRule rule = GPUWidgets::FillRule::NonZero;

        Entry entry;

        // Whether anybody but the publisher has held this slot. Once true the
        // slot is immutable for good, there being no way to ask the holders to
        // give back a uv they have already recorded.
        bool taken = false;
    };

    std::unordered_map<std::uint64_t, Record> entries;

    float scale = 0.f;
    int shared = 0;
};
} // namespace eacp::UI
