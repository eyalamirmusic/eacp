#include "Common.h"

namespace eacp::UI
{
bool contains(const Rect& outer, const Rect& inner)
{
    if (inner.isEmpty())
        return true;

    return inner.left() >= outer.left() && inner.right() <= outer.right()
           && inner.top() >= outer.top() && inner.bottom() <= outer.bottom();
}
} // namespace eacp::UI
