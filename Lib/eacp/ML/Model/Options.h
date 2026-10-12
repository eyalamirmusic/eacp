#pragma once

#include "../Common.h"

namespace eacp::ML
{
// Which devices Core ML may place a model's ops on. The engine-only forms
// need macOS 13 / iOS 16, which is what ML::isSupported() reports.
enum class ComputeUnits
{
    all,
    cpuAndNeuralEngine,
    cpuAndGPU,
    cpu
};

// What a cache directory may hold before a compile evicts its least recently
// used models: 2 GiB, room for a product's handful of models and the versions
// an update just replaced. Caches is purgeable, so this only bounds growth.
constexpr auto defaultCacheBudgetBytes = std::uint64_t {2} << 30;

struct Options
{
    ComputeUnits units = ComputeUnits::all;

    // The weights' identity in the compile cache's key. When the name is
    // given, the blob is not hashed: the caller vouches that a name and
    // version pair always means the same bytes. Empty hashes the blob.
    std::string weightsName;
    std::string weightsVersion;

    // Where compiled models are kept. Empty is defaultCacheDirectory().
    FilePath cacheDirectory;

    // What a compile trims the cache directory back to, oldest use first,
    // sparing models a live Model of this process holds and any used in the
    // last hour. 0 never evicts.
    std::uint64_t cacheBudgetBytes = defaultCacheBudgetBytes;
};

// What a load or a prediction came to, in the shape OnlineResource::Result
// has: ok, and an error message when it is not.
struct Result
{
    constexpr explicit operator bool() const { return ok; }

    static Result success();
    static Result failure(const std::string& message);

    bool ok = false;
    std::string error;
};
} // namespace eacp::ML
