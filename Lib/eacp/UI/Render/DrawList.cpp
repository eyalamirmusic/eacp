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
    images.clear();
    clips.clear();
}

void DrawList::append(DrawCommand::Kind kind, int first)
{
    // Only a run of primitives merges. An entry that carries its own range --
    // a mesh, a layer, a clip -- is one command apiece, since a command holding
    // two of them would have to say which, and the range is already in the entry.
    auto mergeable =
        kind == DrawCommand::Kind::Shapes || kind == DrawCommand::Kind::Images;

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

    // A string of spaces, or one whose every glyph the face has nothing to draw
    // for. It advanced the pen and drew nothing, so there is nothing to replay.
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

void DrawList::addImage(const ImageDraw& image)
{
    if (image.image == nullptr)
        return;

    images.add(image);
    append(DrawCommand::Kind::Images, images.size() - 1);
}

void DrawList::addClip(const ClipDraw& clip)
{
    clips.add(clip);
    append(DrawCommand::Kind::Clip, clips.size() - 1);
}
} // namespace eacp::UI
