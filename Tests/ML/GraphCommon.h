#pragma once

// Device-free, so MLGraphTests links eacp-ml-graph alone; where eacp-ml is
// built too, every package the suite builds is compiled and loaded by Core ML.
#include "SuiteCommon.h"

#include <eacp/ML/MIL/Half.h>
#include <eacp/ML/ML.h>

#include <NanoTest/NanoTest.h>

#include <source_location>
#include <string>

namespace MLGraphTesting
{
using eacp::Vector;
using eacp::ML::Bytes;

inline std::string hex(const Bytes& bytes)
{
    constexpr auto digits = "0123456789ABCDEF";
    auto text = std::string {};

    for (auto index = 0; index < bytes.size(); ++index)
    {
        text += index > 0 ? " " : "";
        text += digits[bytes[index] >> 4];
        text += digits[bytes[index] & 0x0f];
    }

    return text;
}

inline Bytes halves(std::initializer_list<float> values)
{
    return eacp::ML::halfBytes(
        eacp::Span<const float> {values.begin(), values.end()});
}

inline Bytes zeroHalves(int count)
{
    auto bytes = Bytes {};
    bytes.resize(count * 2, 0);
    return bytes;
}

inline eacp::ML::Tensor zeroConstant(eacp::ML::Graph& graph,
                                     std::string_view name,
                                     const eacp::ML::Shape& shape)
{
    auto bytes = zeroHalves(static_cast<int>(shape.count()));
    return graph.constant(name, shape, eacp::ML::DType::float16, bytes);
}

inline void
    checkText(const std::string& actual,
              const std::string& expected,
              const std::source_location& location = std::source_location::current())
{
    auto message = "expected:\n" + expected + "actual:\n" + actual;
    nano::check(actual == expected, message, location);
}

#if EACP_HAS_COREML
inline eacp::FilePath coreMLCacheDirectory()
{
    return MLSuiteCommon::scratchRoot() / "graph";
}

inline void expectCoreMLLoads(const eacp::ML::Package& package,
                              const std::source_location& location)
{
    if (!eacp::ML::isSupported())
        return;

    auto options = eacp::ML::Options {};
    options.cacheDirectory = coreMLCacheDirectory();

    auto model = eacp::ML::Model {};
    auto result = model.load(package, options);
    nano::check(result.ok, "Core ML refused the package: " + result.error, location);
}
#else
inline void expectCoreMLLoads(const eacp::ML::Package&, const std::source_location&)
{
}
#endif

// The graph built, and - where Core ML is here - compiled and loaded, so an
// emitter regression fails the suite rather than a later load.
inline eacp::ML::Package buildChecked(
    const eacp::ML::Graph& graph,
    const std::source_location& location = std::source_location::current())
{
    auto firstError = graph.isValid() ? std::string {} : graph.errors()[0];
    nano::check(graph.isValid(), "graph has errors: " + firstError, location);

    auto package = graph.build();
    nano::check(!package.isEmpty(), "build() produced no package", location);

    if (!package.isEmpty())
        expectCoreMLLoads(package, location);

    return package;
}

inline bool failedWith(const eacp::ML::Graph& graph, std::string_view fragment)
{
    for (auto& error: graph.errors())
        if (error.find(fragment) != std::string::npos)
            return true;

    return false;
}
} // namespace MLGraphTesting
