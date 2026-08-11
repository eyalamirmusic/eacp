#pragma once

#include "Gradient.h"

#include <eacp/GPU/GPU.h>

#include <cstdint>
#include <optional>

namespace eacp::UI
{
// One texture holding every gradient, a row of colours apiece. Rows are keyed by
// colours alone - not spread, not axis - and never rewritten, which is what makes
// it safe to upload in the middle of a pass.
class GradientRamps
{
public:
    // The v to sample this gradient's row at, baked on the first call. Negative
    // when the gradient has no stops, or when there is no row left.
    float rowFor(const Gradient& gradient);

    GradientRamps();

    // Uploads the rows baked since the last call; must precede the draw that
    // samples them.
    void commit();

    // Always a real texture, every shape's fragment reading this one.
    const GPU::Texture& getTexture() const { return *texture; }

    int getRowCount() const { return rows.size(); }

    // Gradients there was no row for, each drawn as its flat colour.
    int getDroppedCount() const { return dropped; }

    static constexpr int rampWidth = 256;

    // Fixed rather than grown: growing means a new texture, and a draw already
    // recorded this frame would be holding the old one.
    static constexpr int maxRows = 256;

private:
    struct Row
    {
        Vector<GradientStop> stops;

        // Of the colours, so finding a row is a scan of integers.
        std::uint64_t key = 0;
    };

    void bake(const Row& source, int row);
    static float vForRow(int row);

    std::optional<GPU::Texture> texture;

    Vector<Row> rows;

    // Uncommitted rows are always the newest ones, so a count suffices.
    int uploadedRows = 0;

    // Row-major RGBA, so a commit uploads a range rather than the whole texture.
    Vector<std::uint8_t> pixels;

    int dropped = 0;
};
} // namespace eacp::UI
