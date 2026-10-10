#pragma once

#include "../Component/Component.h"

#include <string>

namespace eacp::UI
{
// A component that takes files dragged in from the OS.
//
// A mixin rather than a member like DragAndDropTarget: a component that takes
// files *is* a drop zone, and the host finds it by asking each component on the
// path under the pointer whether it is one. The deepest one on that path that
// is interested wins, so a sample slot inside a panel that also takes files gets
// the drop over the slot and the panel gets it everywhere else.
//
// The path is the one a click would take -- visible components whose hitTest
// holds the point, front-most sibling first -- without asking for
// setInterceptsMouseClicks: taking a drop and taking a click are separate, as
// they are for DragAndDropTarget. A component that should let drops through to
// what is behind it overrides hitTest.
//
// Paths are UTF-8 and positions are in the component's own space. Enter and
// exit are matched while the drag moves between targets; a drag that is
// dropped is not exited first, so filesDropped is the last call a target gets.
class FileDropTarget
{
public:
    virtual ~FileDropTarget() = default;

    virtual bool isInterestedInFileDrop(const Vector<std::string>& files) = 0;

    virtual void fileDragEnter(const Vector<std::string>&, Point) {}
    virtual void fileDragMove(const Vector<std::string>&, Point) {}
    virtual void fileDragExit(const Vector<std::string>&) {}

    virtual void filesDropped(const Vector<std::string>& files, Point position) = 0;
};
} // namespace eacp::UI
