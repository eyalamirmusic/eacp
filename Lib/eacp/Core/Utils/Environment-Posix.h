#pragma once

#include "Common.h"

namespace eacp
{

// Every NAME=value entry of the process environment, copied while holding the
// lock setEnv and unsetEnv take.
Vector<std::string> copyEnvironmentEntries();

} // namespace eacp
