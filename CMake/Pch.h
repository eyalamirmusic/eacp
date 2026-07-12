#pragma once

// Precompiled once and reused by every eacp target.
//
// Standard-library headers only, on purpose. Adding eacp's own umbrella
// headers here would compile faster still, but a PCH is force-included into
// every translation unit: a missing #include inside eacp would then be
// silently satisfied here and only surface for a downstream user who never
// sees this file.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <eacp/Core/Core.h>
