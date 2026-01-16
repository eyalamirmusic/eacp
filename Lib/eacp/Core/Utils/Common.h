#pragma once

#include <functional>
#include "Pimpl.h"
#include "Logging.h"

namespace eacp
{
using Callback = std::function<void()>;
}