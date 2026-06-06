#pragma once

// Single shared header that pulls in the EA container types eacp depends on
// and surfaces the most-used ones inside the eacp namespace as
// eacp::Vector / eacp::Array / eacp::OwningPointer / eacp::OwnedVector
// (plus the makeOwned factory). Code inside any eacp::* namespace can then
// use the unqualified names instead of EA::Vector, etc.
//
// Mirrors the existing Broadcaster.h pattern. Include this (directly or
// transitively) wherever an EA container is needed rather than the
// individual ea_data_structures headers.

#include <ea_data_structures/Pointers/OwningPointer.h>
#include <ea_data_structures/Structures/Array.h>
#include <ea_data_structures/Structures/OwnedVector.h>
#include <ea_data_structures/Structures/Vector.h>

namespace eacp
{
using EA::Array;
using EA::makeOwned;
using EA::OwnedVector;
using EA::OwningPointer;
using EA::Vector;
} // namespace eacp
