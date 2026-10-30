#pragma once

#include "PrecompiledNames.h"

#include <ResEmbed/ResEmbed.h>

#include <cstdint>
#include <string>
#include <string_view>

// The runtime half of ahead-of-time shader compilation: a ShaderLibrary looks
// for the native binary a build compiled from the very source it was handed,
// embedded under resourceCategory as <keyFor(source)><suffix>
// (PrecompiledNames.h), and loads that instead of compiling the text. A miss,
// or a binary that will not load, falls back to the source compile unless
// EACP_REQUIRE_PRECOMPILED_SHADERS=1.
namespace eacp::GPU::Precompiled
{
// Bytes injected with setForTesting win over embedded ones. Empty on a miss.
ResEmbed::DataView find(std::string_view resourceName);

// Injected bytes stay alive until the process exits, cleared or not, so a
// library built from them never outlives what it was built from.
void setForTesting(std::string resourceName, Vector<std::uint8_t> bytes);

// Drops every injected resource and the required override.
void clearForTesting();

// EACP_REQUIRE_PRECOMPILED_SHADERS=1, unless setRequiredForTesting overrode it.
bool isRequired();
void setRequiredForTesting(bool required);

// True when anything at all is precompiled - embedded or injected. A miss in
// an app that embeds nothing is the expected case, not one worth a log line.
bool isAvailable();

// How many blobs the app embeds under resourceCategory; injected ones are not
// counted.
int embeddedCount();

// One log line per resource name for the life of the process.
void reportMiss(std::string_view resourceName, std::string_view reason);

// Whether a library that did not find `resourceName` should compile its source.
// False under the require flag, which says so in the log.
bool fallBackAfterMiss(std::string_view resourceName);

// The same for one that found it and could not load it.
bool fallBackAfterLoadFailure(std::string_view resourceName,
                              std::string_view reason);
} // namespace eacp::GPU::Precompiled
