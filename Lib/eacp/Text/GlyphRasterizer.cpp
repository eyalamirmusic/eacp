#include "GlyphRasterizer.h"

namespace eacp::Text
{
FontMetrics GlyphRasterizer::metrics(FontStyle style) const
{
    return metrics(variantOf(style));
}
} // namespace eacp::Text
