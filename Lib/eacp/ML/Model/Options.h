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
};

// What a load or a prediction came to, in the shape OnlineResource::Result
// has: ok, and an error message when it is not.
struct Result
{
    explicit operator bool() const { return ok; }

    static Result success() { return {true, {}}; }
    static Result failure(const std::string& message) { return {false, message}; }

    bool ok = false;
    std::string error;
};
} // namespace eacp::ML
