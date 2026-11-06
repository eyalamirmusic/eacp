#pragma once

/** @file Internal: Linux and Android only: the command list a Path records there. */

#include "Path.h"

namespace eacp::Graphics
{

// One step of the transcript a Linux Path keeps; arcs are recorded as cubics.
struct PathCommand
{
    enum class Verb
    {
        move,
        line,
        quad,
        cubic,
        close
    };

    // Control points come first and the on-curve endpoint last.
    int pointCount() const;

    Verb verb = Verb::move;
    Point points[3] {};
};

// What Path::getHandle() points at on Linux.
struct PathGeometry
{
    Vector<PathCommand> commands;
};

// The commands recorded so far; valid while the Path lives and is unchanged.
const PathGeometry& getPathGeometry(const Path& path);

} // namespace eacp::Graphics
