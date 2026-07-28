#pragma once

#include "../Component/Component.h"

#include <optional>

namespace eacp::UI
{
// The one native view a whole component tree lives in.
//
// A GPUView, so it sits in the ordinary eacp::Graphics::View hierarchy and
// reuses its window, sizing and events -- and so a component tree can be one
// leaf of a larger native layout, or fill a window, or be embedded in a host
// application through Graphics::EmbeddedView, without knowing which.
//
// It owns the renderers the painter draws through and the root component, and
// it converts the native view's events into component-local ones. Rendering is
// the View's ordinary on-demand cycle: Component::repaint marks the tree dirty
// and the next draw walks it. Nothing is submitted while nothing is dirty.
class ComponentHost : public GPU::GPUView
{
public:
    ComponentHost();
    ~ComponentHost() override;

    // The tree to draw. The component is not owned and has to outlive the host.
    // It is resized to fill the host, so its own bounds are ignored.
    void setRootComponent(Component& newRoot);
    Component* getRootComponent() const { return root; }

    void setBackgroundColour(const Color& colour);

    void setFontPointSize(float points);
    void setFontFamily(const std::string& family);

    // What the last frame cost. `clipChanges` is the number of batch breaks:
    // between two of them every quad goes out as one instanced draw, so this is
    // the figure that should stay flat as the tree grows.
    int getLastClipChangeCount() const { return lastClipChanges; }
    int getLastComponentCount() const { return lastComponentCount; }

    void resized() override;
    void render(GPU::Frame& frame) override;

    void mouseDown(const eacp::Graphics::MouseEvent& event) override;
    void mouseUp(const eacp::Graphics::MouseEvent& event) override;
    void mouseDragged(const eacp::Graphics::MouseEvent& event) override;
    void mouseMoved(const eacp::Graphics::MouseEvent& event) override;
    void mouseExited(const eacp::Graphics::MouseEvent& event) override;
    void mouseWheel(const eacp::Graphics::MouseEvent& event) override;

private:
    friend class Component;

    // A component in this tree is going away. Drops every pointer the host
    // holds to it -- the root, the hover, the press capture -- because a host
    // outliving its tree by even one destructor is the normal case: a root held
    // as a member of a ComponentHost subclass is destroyed before the host's
    // own base, so the host would otherwise write through a dead pointer on its
    // way out.
    void componentDeleted(Component& component);

    void paintComponent(Component& component, Graphics& g);

    // Rasterizing every PathShape in the tree whose geometry changed, before
    // the render pass opens. See the definition for why it can only happen here.
    void rasterizePaths(GPU::Frame& frame);
    void rasterizeDirtyPaths(Component& component,
                             GPU::ComputePass& pass,
                             bool& atlasMoved);
    void markAllPathsDirty(Component& component);

    // Builds the event a component sees: the position converted into its own
    // space, and the fields the native event already carries.
    MouseEvent makeEvent(const Component& target,
                         const eacp::Graphics::MouseEvent& event) const;

    void updateHover(Component* target, const eacp::Graphics::MouseEvent& event);
    void setHoveredComponent(Component* component,
                             const eacp::Graphics::MouseEvent& event);

    Component* root = nullptr;

    Color background {0.11f, 0.12f, 0.15f, 1.f};

    std::string fontFamily {defaultUIFontFamily()};
    float fontPointSize = 13.f;

    // Built on the first resize, once there is a size to build them against.
    // The atlas comes first and outlives the batch that reads it.
    std::optional<CoverageAtlas> paths;
    std::optional<ShapeBatch> shapes;
    std::optional<Text::TextRenderer> text;

    // The scale everything in the atlas was rasterized at, so a move between
    // displays can be noticed.
    float lastPathScale = 0.f;

    // Where the button went down, in root space, carried through the drag so
    // every event can report it.
    Point dragOrigin;
    Point lastMousePosition;

    // The component a press captured. Drags and the matching release go to it
    // wherever the pointer has since travelled -- a fader keeps tracking once
    // the pointer leaves it, which is the behaviour a drag has to have.
    Component* mouseDownTarget = nullptr;
    Component* hoveredComponent = nullptr;

    int lastClipChanges = 0;
    int lastComponentCount = 0;
};
} // namespace eacp::UI
