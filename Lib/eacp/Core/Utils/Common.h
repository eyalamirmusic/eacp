#pragma once

#include "Containers.h"
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
using Callback = std::function<void()>;
}
