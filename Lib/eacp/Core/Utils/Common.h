#pragma once

/** @file Umbrella of containers, strings, format, logging, Pimpl and Time. */

#include "Containers.h"
#include "Format.h"
#include "Logging.h"
#include "Pimpl.h"
#include "Strings.h"
#include "Time.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eacp
{
/// A void() callback, the type every eacp API that schedules work takes.
using Callback = std::function<void()>;
} // namespace eacp
