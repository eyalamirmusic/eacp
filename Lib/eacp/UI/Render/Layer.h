#pragma once

#include "../Common.h"

#include <eacp/GPU/GPU.h>

#include <functional>
#include <optional>

namespace eacp::UI
{
class Component;
class Graphics;

// A run of drawing rendered into a texture of its own, so it composites as one
// thing (SVG group opacity). Must be a member of the component that draws it,
// and a layer holding another must be constructed after the one it holds.
class Layer
{
public:
    explicit Layer(Component& ownerToUse);
    ~Layer();

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    // In the owning component's points; the texture costs this area rounded out
    // to whole device pixels.
    void setBounds(const Rect& newBounds);
    Rect getBounds() const { return bounds; }

    // Multiplied into the whole layer as it is composited.
    void setOpacity(float newOpacity);
    float getOpacity() const { return opacity; }

    // Drawn with the origin at the layer's own top-left. Called by the host,
    // before the frame's pass, and only when the layer is dirty.
    std::function<void(Graphics&)> onPaint = [](Graphics&) {};

    // Setting the bounds does this; a change of what onPaint would draw does not.
    void setDirty();

    // Drawing an empty layer is a no-op.
    bool isEmpty() const { return !ready; }

    const GPU::Texture& getTexture() const { return *texture; }

    // Normalised, and not the whole texture: it is kept across a shrink.
    Rect getContentUV() const;

private:
    friend class ComponentHost;

    bool isDirty() const { return dirty; }

    // False when the bounds hold no pixels or the device gave no texture, and
    // the layer then draws as nothing. Only remade when it is too small.
    bool ensureTexture(float scale);

    void markRendered();

    Component& owner;

    Rect bounds;
    float opacity = 1.f;

    std::optional<GPU::Texture> texture;

    // In texels; the texture may be larger, being kept across a shrink.
    int contentWidth = 0;
    int contentHeight = 0;

    float renderedScale = 0.f;

    bool dirty = true;
    bool ready = false;
};
} // namespace eacp::UI
