#pragma once

/** @file Re-exports EA::Broadcaster and EA::Listener into eacp. */

#include <ea_data_structures/Pointers/Broadcaster.h>

namespace eacp
{
using EA::Broadcaster;
using EA::Listener;
} // namespace eacp
