#pragma once

#include "Gradient.h"

#include <eacp/GPU/GPU.h>

#include <cstdint>
#include <optional>

namespace eacp::UI
{
// One texture holding every gradient the interface draws, a row of colours
// apiece, so that a gradient-filled shape costs a fetch rather than a pipeline.
//
// The same argument CoverageAtlas makes about masks, made about colours. A
// gradient evaluated from its stops in the fragment stage would mean a loop over
// a stop list, a uniform block per gradient, and a batch break between two
// shapes filled differently. Baked into a row of 256 texels it is a texture read
// at a coordinate the fragment computes, so a document of fifty gradients draws
// in one instanced draw and the shader has no idea how many stops any of them
// had.
//
// A row is keyed by the colours in it, not by the shape that asked: two shapes
// with the same stops and different axes share one row, which is what a document
// repeating one gradient across an illustration does. Spread is not part of the
// key either -- the shader wraps the coordinate before it reads, so pad, reflect
// and repeat are the same row read three ways.
//
// Rows are written once and never rewritten, which is what makes it safe to
// upload in the middle of a pass: an upload cannot disturb a draw already
// recorded, because nothing a recorded draw reads is ever the thing being
// written. That is how the glyph atlas behaves too, and for the same reason.
class GradientRamps
{
public:
    // Which row of the texture holds this gradient's colours, as the v to sample
    // at. Negative when the gradient has no stops, or when there is no row left.
    // Only the row varies: every ramp runs the full width, so where along it a
    // fragment reads is arithmetic the shader does from a constant.
    //
    // Baked on the first call and found on every later one, so calling it per
    // shape per frame is the intended use.
    float rowFor(const Gradient& gradient);

    GradientRamps();

    // Uploads the rows baked since the last call. Called before the draw that
    // samples them; cheap and a no-op once an interface's gradients are known,
    // which for a document is after its first frame.
    void commit();

    // Always a real texture, including for an interface that never draws a
    // gradient. A sampler is not something a pipeline can leave empty and every
    // shape's fragment reads this one, so the alternative is a stand-in texture
    // and a branch to choose it -- against a quarter of a megabyte that is fixed
    // whatever the interface does.
    const GPU::Texture& getTexture() const { return *texture; }

    int getRowCount() const { return rows.size(); }

    // Gradients there was no row for. Each one draws as its flat colour, which
    // is a picture missing its shading rather than missing a shape -- but it is
    // still a thing that happened silently, so it is counted.
    int getDroppedCount() const { return dropped; }

    // How many texels wide a ramp is. Enough that a two-stop gradient across a
    // full window has more steps than an 8-bit channel can show, and a document
    // of many-stop gradients still resolves every stop it has.
    static constexpr int rampWidth = 256;

    // How many gradients an interface may have at once. Fixed rather than grown,
    // because growing means a new texture, and a draw already recorded this
    // frame would be holding the old one.
    static constexpr int maxRows = 256;

private:
    struct Row
    {
        Vector<GradientStop> stops;
    };

    void bake(const Row& source, int row);
    static float vForRow(int row);

    std::optional<GPU::Texture> texture;

    Vector<Row> rows;

    // The rows baked since the last commit, as a half-open range: they are
    // always the newest ones, so a range is all the bookkeeping there is.
    int uploadedRows = 0;

    // Row-major RGBA, kept on the CPU so a commit uploads a range of rows out of
    // it rather than the whole texture.
    Vector<std::uint8_t> pixels;

    int dropped = 0;
};
} // namespace eacp::UI
