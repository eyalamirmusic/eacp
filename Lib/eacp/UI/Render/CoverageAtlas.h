#pragma once

#include "../Common.h"

#include <eacp/GPUWidgets/GPUWidgets.h>

#include <cstdint>

namespace eacp::UI
{
// One texture every path rasterizes into, so drawing a path costs a quad. Texel
// (0, 0) is opaque, and unmasked shapes sample it to stay on one pipeline.
// Allocation is a shelf with no free list; see makeRoomFor and takeMovedFlag.
class CoverageAtlas
{
public:
    // A rect of the atlas and the uv rect sampling it. A failed allocation
    // yields a zero-width slot, which the caller must test for.
    struct Slot
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        Rect uv;
    };

    CoverageAtlas();

    const GPU::Texture& getTexture() const { return *texture; }

    // What a shape with no mask samples: a uv rect inside the opaque texel.
    Rect getOpaqueUV() const;

    // `width` and `height` are device pixels. Yields a zero-width slot when
    // there is nowhere it can go.
    Slot allocate(int width, int height);

    // Whether an allocation may grow or compact to make room. Must be false on
    // the pass whose layout is being drawn through; an allocation that would
    // have moved something is then refused and counted instead.
    void setRelocationAllowed(bool allowed) { relocationAllowed = allowed; }

    // Only sound once nothing holds a slot any more - every shape must have
    // been invalidated first.
    void forgetAllocations() { reset(); }

    // The uv rect for part of a slot, so a mask smaller than the room reserved
    // for it can stay where it is.
    Rect uvFor(int x, int y, int width, int height) const;

    // True when the last allocate() grew or compacted. Cleared by the reader.
    bool takeMovedFlag();

    // Bumped whenever every uv already handed out stops meaning what it did -
    // for callers that recorded a uv rather than asking for one each frame.
    std::uint32_t generation() const { return atlasGeneration; }

    // Masks there was no room for, each of which draws as nothing.
    int getDroppedCount() const { return dropped; }
    void clearDroppedCount() { dropped = 0; }

    // Room reserved rather than coverage written: the shelf never gives back.
    float getFillFraction() const;

    int getWidth() const;
    int getHeight() const;

private:
    std::optional<Slot> placeOnShelf(int width, int height);
    bool makeRoomFor(int width, int height);

    bool isShelfEmpty() const;

    void resizeTo(int size);
    void seedOpaqueTexel();

    // Forgets every allocation. The texture and its opaque texel survive.
    void reset();

    std::optional<GPU::Texture> texture;

    // The shelf cursor: where the next slot goes, and how far down the next row
    // starts.
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;

    // Including the opaque corner, which no allocation produced.
    std::int64_t usedTexels = 0;

    bool relocationAllowed = true;
    bool moved = false;
    int dropped = 0;

    std::uint32_t atlasGeneration = 0;
};
} // namespace eacp::UI
