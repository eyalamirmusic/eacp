#include "LinuxWindowSurface-Linux.h"

#include "../View/View.h"

#include <algorithm>

namespace eacp::Graphics
{
namespace
{
MouseEvent linuxEventIn(const View& root, MouseEvent event)
{
    auto origin = root.getBounds();

    event.pos = {event.pos.x - origin.x, event.pos.y - origin.y};
    event.downPos = {event.downPos.x - origin.x, event.downPos.y - origin.y};

    return event;
}

bool linuxIsPointerType(MouseEventType type)
{
    return type == MouseEventType::Moved || type == MouseEventType::Entered;
}
} // namespace

Rect LinuxWindowSurface::contentBounds() const
{
    auto height = std::max(contentSize.y - contentTop, 0.f);
    return {0.f, contentTop, contentSize.x, height};
}

void LinuxWindowSurface::layoutViews()
{
    if (contentView != nullptr)
        contentView->setBounds(contentBounds());

    layoutMenuBar();
}

View* LinuxWindowSurface::rootAt(Point position) const
{
    if (menuBarView != nullptr && menuBarView->getBounds().contains(position))
        return menuBarView;

    return menuBarModal ? nullptr : contentView;
}

void LinuxWindowSurface::dispatchMouseEvent(const MouseEvent& event)
{
    if (event.type == MouseEventType::Dragged || event.type == MouseEventType::Up)
    {
        // A press that began outside the window still releases in the content,
        // as it did before there was anything else to route to; one that
        // closed a menu releases nowhere.
        auto* target = pressRoot;

        if (target == nullptr && !pressSwallowed)
            target = contentView;

        if (event.type == MouseEventType::Up)
        {
            pressRoot = nullptr;
            pressSwallowed = false;
        }

        if (target != nullptr)
            target->dispatchMouseEvent(linuxEventIn(*target, event));

        return;
    }

    if (event.type == MouseEventType::Exited)
    {
        // With no menu bar, the content hears every exit, as it always did.
        auto* left = hoverRoot;

        if (left == nullptr && menuBarView == nullptr)
            left = contentView;

        hoverRoot = nullptr;

        if (left != nullptr)
            left->dispatchMouseEvent(linuxEventIn(*left, event));

        return;
    }

    auto* target = rootAt(event.pos);

    if (linuxIsPointerType(event.type) && target != hoverRoot)
    {
        if (auto* left = hoverRoot)
        {
            auto exited = event;
            exited.type = MouseEventType::Exited;

            hoverRoot = nullptr;
            left->dispatchMouseEvent(linuxEventIn(*left, exited));
        }

        hoverRoot = target;
    }

    if (target == nullptr)
    {
        // Last: closing the menu changes what this window routes to.
        if (event.type == MouseEventType::Down && menuBarModal)
        {
            pressRoot = nullptr;
            pressSwallowed = true;

            auto dismiss = menuBarDismiss;
            dismiss();
        }

        return;
    }

    if (event.type == MouseEventType::Down)
    {
        pressRoot = target;
        pressSwallowed = false;
    }

    target->dispatchMouseEvent(linuxEventIn(*target, event));
}

void LinuxWindowSurface::dispatchKeyEvent(const KeyEvent& event)
{
    if (keyFilter)
    {
        // Returns at once when the key was kept: what it ran may have
        // destroyed this window.
        auto filter = keyFilter;

        if (filter(event))
            return;
    }

    if (contentView != nullptr)
        contentView->dispatchKeyEvent(event);
}

MouseCursor::Shape LinuxWindowSurface::cursorShapeAt(Point position)
{
    auto* root = rootAt(position);

    if (root == nullptr)
        return MouseCursor::Default;

    auto origin = root->getBounds();
    auto* hit = root->hitTest({position.x - origin.x, position.y - origin.y});

    return hit != nullptr ? hit->getMouseCursor().getShape()
                          : root->getMouseCursor().getShape();
}

void LinuxWindowSurface::forgetRoot(const View* view)
{
    if (hoverRoot == view)
        hoverRoot = nullptr;

    if (pressRoot == view)
        pressRoot = nullptr;
}
} // namespace eacp::Graphics
