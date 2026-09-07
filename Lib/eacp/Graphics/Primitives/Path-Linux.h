#pragma once

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

const PathGeometry& getPathGeometry(const Path& path);

} // namespace eacp::Graphics
