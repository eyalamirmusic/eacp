#include "ScopedAutoReleasePool.h"

namespace eacp
{
// Platforms with no autorelease pool: the object exists so portable code can
// scope one unconditionally.
ScopedAutoReleasePool::ScopedAutoReleasePool() = default;
ScopedAutoReleasePool::~ScopedAutoReleasePool() = default;
} // namespace eacp
