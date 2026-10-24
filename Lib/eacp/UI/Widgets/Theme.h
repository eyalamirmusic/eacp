#pragma once

#include "../Common.h"

namespace eacp::UI
{
// The colours the stock widgets draw with. One struct rather than a setter per
// colour on each widget, so a theme is a value that can be shared, copied and
// swapped rather than a hundred assignments -- the job JUCE gives LookAndFeel,
// at the scale this prototype needs.
//
// A tree's widgets draw with their host's (ComponentHost::setTheme), which is
// this default until it is told otherwise -- the way they draw in the host's
// font. A colour a widget was given itself, such as setAccentColour, wins.
struct Theme
{
    Color background {0.11f, 0.12f, 0.15f, 1.f};
    Color panel {0.16f, 0.17f, 0.21f, 1.f};
    Color outline {1.f, 1.f, 1.f, 0.10f};
    Color text {0.92f, 0.93f, 0.96f, 1.f};
    Color dimText {0.60f, 0.63f, 0.70f, 1.f};
    Color accent {0.35f, 0.62f, 0.95f, 1.f};
    Color accentDim {0.35f, 0.62f, 0.95f, 0.35f};
    Color hover {1.f, 1.f, 1.f, 0.07f};
    Color pressed {1.f, 1.f, 1.f, 0.14f};
};

const Theme& defaultTheme();
} // namespace eacp::UI
