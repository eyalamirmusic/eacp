#pragma once

/** @file
    ShaderLibrary, a ShaderSource compiled on a Device, or loaded from a binary
    compiled at build time.
*/

#include "../Common.h"

#include "CompilationInfo.h"
#include "PipelineConstants.h"
#include "ShaderSource.h"

namespace eacp::GPU
{
class Device;

// A compiled shader library (MTLLibrary on Metal). Holds the entry-point names
// it was built with so a pipeline only needs to reference the library.
class ShaderLibrary
{
public:
    // Loads the build-time binary for `source` when one is embedded, else
    // compiles the source on `device`. Failure, an invalid device or an empty
    // source leaves isValid() false; nothing throws.
    ShaderLibrary(Device& device, const ShaderSource& source);

    const std::string& vertexEntry() const;
    const std::string& fragmentEntry() const;
    const std::string& computeEntry() const;

    // False for a vertex-only library (ShaderSource::withFragment with an empty
    // entry), whose pipelines have no fragment stage.
    bool hasFragment() const;

    // The group the kernel was emitted for, carried to the pipeline the pass
    // dispatches through.
    ThreadGroupShape threadGroupShape() const;

    bool isValid() const;

    // What compiling the library said: the compiler's errors and warnings, each
    // stage's where a backend compiles them apart, positioned in the source it
    // was given, and anything the library reports of itself - no device, an
    // empty source, a precompiled binary required and missing. An invalid
    // library always holds an error; a valid one may hold warnings. Empty for a
    // library loaded precompiled, and for a clean compile. The compiler's text
    // is still logged as it was. Metal drops warnings from a library it builds,
    // so there they come only beside an error.
    const CompilationInfo& compilationInfo() const;

    // The same for the variant `constants` builds: on D3D, where each set of
    // constants is a compile of its own, that compile's messages, compiling it
    // now if no pipeline has asked for it yet; the library's own everywhere
    // else, and for the empty set.
    CompilationInfo compilationInfo(const PipelineConstants& constants) const;

    // True when the library was loaded from a binary compiled ahead of time
    // (PrecompiledShaders.h) rather than compiled from its source at runtime.
    bool isPrecompiled() const;

    // Whether the source declares pipeline-overridable constants
    // (ShaderSource::withPipelineConstants), which a pipeline specialises.
    bool hasPipelineConstants() const;

    // Opaque native handle for cross-translation-unit use by the pipeline.
    void* nativeLibrary() const;

    // The handle a pipeline specialised with `constants` is built from. The
    // library itself on Metal and Vulkan, whose pipelines specialise one
    // compiled library. On D3D, where FXC has none, the variant of the source
    // compiled with those constants' EACP_OVERRIDE_<id> macros, compiled on
    // first use and kept for the library's life; the empty set is the library
    // itself - the one a precompiled blob holds. Null when the variant does not
    // compile.
    void* nativeLibrary(const PipelineConstants& constants) const;

private:
    std::string vertexEntryName;
    std::string fragmentEntryName;
    std::string computeEntryName;
    ThreadGroupShape groupShape;
    bool pipelineConstants = false;
    CompilationInfo compilation;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
