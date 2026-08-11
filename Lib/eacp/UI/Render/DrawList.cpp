#include "DrawList.h"

namespace eacp::UI
{
void DrawList::clear()
{
    commands.clear();
    shapes.clear();
    glyphs.clear();
    glyphRuns.clear();
    meshes.clear();
    layers.clear();
    clips.clear();
}

void DrawList::append(DrawCommand::Kind kind, int first)
{
    // Entries carrying their own range - mesh, layer, clip - get one command
    // apiece.
    auto mergeable = kind == DrawCommand::Kind::Shapes;

    if (mergeable && !commands.empty())
    {
        auto& last = commands.back();

        if (last.kind == kind && last.first + last.count == first)
        {
            ++last.count;
            return;
        }
    }

    commands.add({kind, first, 1});
}

void DrawList::addShape(const ShapeDraw& shape)
{
    shapes.add(shape);
    append(DrawCommand::Kind::Shapes, shapes.size() - 1);
}

void DrawList::addGlyphRun(int first, const Color& colour)
{
    auto count = glyphs.size() - first;

    // A string of spaces advanced the pen and drew nothing.
    if (count <= 0)
        return;

    glyphRuns.add({first, count, colour});
    append(DrawCommand::Kind::Glyphs, glyphRuns.size() - 1);
}

void DrawList::addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                       Point offset,
                       const Rect& bounds,
                       const Color& colour,
                       const GradientFill& gradient)
{
    if (mesh.empty())
        return;

    meshes.add({mesh, offset, bounds, colour, gradient});
    append(DrawCommand::Kind::Mesh, meshes.size() - 1);
}

void DrawList::addLayer(const Layer& layer, const Rect& bounds)
{
    layers.add({&layer, bounds});
    append(DrawCommand::Kind::Layer, layers.size() - 1);
}

void DrawList::addClip(const ClipDraw& clip)
{
    clips.add(clip);
    append(DrawCommand::Kind::Clip, clips.size() - 1);
}
} // namespace eacp::UI
