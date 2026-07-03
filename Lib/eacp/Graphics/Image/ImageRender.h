#pragma once

#include "Image.h"
#include "../Graphics/GraphicsContext.h"

#include <functional>

namespace eacp::Graphics
{

// Rasterizes ad-hoc Context drawing into a straight-alpha RGBA Image of
// exactly width x height pixels, without needing a window or view. The
// context uses the same top-left-origin coordinate space as View::paint
// and starts fully transparent. Returns an invalid image on non-positive
// dimensions or when no rendering backend is available.
Image renderToImage(int width,
                    int height,
                    const std::function<void(Context&)>& draw);

} // namespace eacp::Graphics
