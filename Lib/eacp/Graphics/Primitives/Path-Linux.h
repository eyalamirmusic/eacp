#pragma once

#include "Path.h"

namespace eacp::Graphics
{

// One step of the transcript a Linux Path keeps.
//
// There is no arc verb on purpose. Core Graphics and Direct2D both have one and
// each spells it differently — a rect plus corner radii there, an endpoint plus
// a sweep direction here — while the only two callers that want arcs,
// addRoundedRect and addEllipse, can say the same thing in cubics to within
// about 0.02% of the radius. One fewer verb is one fewer case in every consumer
// that walks this.
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

    // How many of `points` this verb uses. Control points come first and the
    // on-curve endpoint last, so the pen always lands on
    // points[pointCount() - 1], and `close` uses none of them.
    int pointCount() const;

    Verb verb = Verb::move;
    Point points[3] {};
};

// What Path::getHandle() points at on Linux: cast the void* back to this, or
// call getPathGeometry() below and skip the cast.
struct PathGeometry
{
    Vector<PathCommand> commands;
};

const PathGeometry& getPathGeometry(const Path& path);

} // namespace eacp::Graphics
