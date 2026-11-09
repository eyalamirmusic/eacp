#pragma once

#include <eacp/UI/UI.h>

struct LayoutTargets
{
    eacp::UI::Component& title;
    eacp::UI::Component& sidebar;
    eacp::UI::Component& gain;
    eacp::UI::Component& mute;
    eacp::UI::Component& notes;
    eacp::UI::Component& footer;
};

using LayoutFunction = void(const LayoutTargets& targets,
                            const eacp::UI::Rect& bounds);
