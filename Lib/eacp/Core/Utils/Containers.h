#pragma once

// Pulls in the EA container types eacp depends on and re-exports the most-used
// ones into the eacp namespace (Vector / Array / OwningPointer / OwnedVector,
// plus makeOwned), so eacp::* code can use the unqualified names instead of
// EA::Vector, etc. Include this rather than the individual ea_data_structures
// headers.

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
