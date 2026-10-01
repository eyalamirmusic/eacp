#pragma once

#include "SuiteCommon.h"
#include "TestPrograms.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/StdPath.h>
#include <eacp/ML/ML.h>

#include <NanoTest/NanoTest.h>

#include <TargetConditionals.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sys/sysctl.h>

namespace ModelTests
{
using eacp::Array;
using eacp::FilePath;
using eacp::getEnvValue;
using eacp::LOG;
using eacp::toStdPath;
using eacp::Vector;
using MLSuiteCommon::arrayOf;
using namespace eacp::ML;

inline bool isAneRequired()
{
    return getEnvValue("EACP_REQUIRE_ANE") == "1";
}

constexpr bool isIOS()
{
    return TARGET_OS_IPHONE != 0;
}

struct Version
{
    int major = 0;
    int minor = 0;

    bool atLeast(int wantedMajor, int wantedMinor) const
    {
        return major > wantedMajor || (major == wantedMajor && minor >= wantedMinor);
    }
};

inline Version osVersion()
{
    auto text = Array<char, 64> {};
    auto size = text.getSize();
    sysctlbyname("kern.osproductversion", text.data(), &size, nullptr, 0);

    auto version = Version {};
    std::sscanf(text.data(), "%d.%d", &version.major, &version.minor);
    return version;
}

inline FilePath freshCacheDirectory(const std::string& name)
{
    return MLSuiteCommon::freshScratchDirectory(name);
}

inline Vector<std::string> entriesOf(const FilePath& directory)
{
    auto names = Vector<std::string> {};
    auto error = std::error_code {};

    for (const auto& entry:
         std::filesystem::directory_iterator {toStdPath(directory), error})
        names.add(entry.path().filename().string());

    return names;
}

inline Options optionsFor(ComputeUnits units, const FilePath& cacheDirectory)
{
    auto options = Options {};
    options.units = units;
    options.cacheDirectory = cacheDirectory;
    return options;
}

inline std::string nameOf(ComputeUnits units)
{
    switch (units)
    {
        case ComputeUnits::all:
            return "all";
        case ComputeUnits::cpuAndNeuralEngine:
            return "cpuAndNeuralEngine";
        case ComputeUnits::cpuAndGPU:
            return "cpuAndGPU";
        case ComputeUnits::cpu:
            return "cpu";
    }

    return "?";
}

struct Errors
{
    double maxAbs = 0.0;
    double maxRel = 0.0;
};

// Relative error only where the reference is at least 1e-4, as the spike
// measured it: a softmax is mostly tiny values whose relative error is noise.
inline Errors compare(const Vector<float>& actual, const Vector<float>& expected)
{
    auto errors = Errors {};

    if (actual.size() != expected.size())
        return {INFINITY, INFINITY};

    for (auto i = 0; i < actual.size(); ++i)
    {
        auto difference = std::abs((double) actual[i] - (double) expected[i]);

        if (std::isnan(difference))
            return {INFINITY, INFINITY};

        errors.maxAbs = std::max(errors.maxAbs, difference);

        if (std::abs(expected[i]) >= 1e-4f)
            errors.maxRel =
                std::max(errors.maxRel, difference / std::abs((double) expected[i]));
    }

    return errors;
}

inline std::string placementOf(const Model& model)
{
    auto text = std::string {};

    for (const auto& op: model.computePlan().ops)
        text += (text.empty() ? "" : " ") + op.type + ":" + toString(op.device);

    return text.empty() ? "(no plan)" : text;
}
} // namespace ModelTests
