#pragma once

#include "Theme.h"

// The keys eacp's own components read, with defaultTheme() holding a value for
// each that has one. Any other library or app declares its keys the same way,
// beside the code that draws with them, and sets them in the same Theme.
namespace eacp::UI::ThemeKeys
{
// Roles: what the stock widgets share, so a set of controls agrees.
inline constexpr ColourKey background {"background"};
inline constexpr ColourKey panel {"panel"};
inline constexpr ColourKey outline {"outline"};
inline constexpr ColourKey text {"text"};
inline constexpr ColourKey dimText {"dimText"};
inline constexpr ColourKey accent {"accent"};
inline constexpr ColourKey accentDim {"accentDim"};

// Text drawn on an accent fill: a toggled button's, the current tab's.
inline constexpr ColourKey accentText {"accentText"};

inline constexpr ColourKey hover {"hover"};
inline constexpr ColourKey pressed {"pressed"};

// The host's face. A theme that holds one gives it to the host it is set on,
// as ComponentHost::setFont would.
inline constexpr FontKey font {"font"};

// What one widget draws that the roles do not cover. Unset in defaultTheme()
// where the widget falls back to a role instead, as the combo box's highlight
// is accentDim until a theme says otherwise.
inline constexpr NumberKey buttonCorner {"button.cornerRadius"};
inline constexpr NumberKey checkboxCorner {"checkbox.cornerRadius"};
inline constexpr NumberKey comboBoxCorner {"comboBox.cornerRadius"};
inline constexpr ColourKey comboBoxHighlight {"comboBox.highlight"};
inline constexpr NumberKey sliderCorner {"slider.cornerRadius"};
inline constexpr NumberKey tabCorner {"tabs.cornerRadius"};
inline constexpr NumberKey textEditorCorner {"textEditor.cornerRadius"};
} // namespace eacp::UI::ThemeKeys
