#include "ThemeKeys.h"

namespace eacp::UI
{
void Theme::setValue(std::string_view name, std::uint64_t id, ThemeValue value)
{
    entries.insert_or_assign(id, Entry {std::string(name), std::move(value)});
}

const ThemeValue* Theme::findValue(std::string_view name, std::uint64_t id) const
{
    auto found = entries.find(id);

    if (found == entries.end() || found->second.name != name)
        return nullptr;

    return &found->second.value;
}

namespace
{
// The colours, face and corners eacp has always drawn in, so an app that never
// sets a theme looks as it did before there were themes.
Theme makeDefaultTheme()
{
    auto theme = Theme {};

    theme.set(ThemeKeys::background, {0.11f, 0.12f, 0.15f, 1.f});
    theme.set(ThemeKeys::panel, {0.16f, 0.17f, 0.21f, 1.f});
    theme.set(ThemeKeys::outline, {1.f, 1.f, 1.f, 0.10f});
    theme.set(ThemeKeys::text, {0.92f, 0.93f, 0.96f, 1.f});
    theme.set(ThemeKeys::dimText, {0.60f, 0.63f, 0.70f, 1.f});
    theme.set(ThemeKeys::accent, {0.35f, 0.62f, 0.95f, 1.f});
    theme.set(ThemeKeys::accentDim, {0.35f, 0.62f, 0.95f, 0.35f});
    theme.set(ThemeKeys::accentText, {0.08f, 0.09f, 0.12f, 1.f});
    theme.set(ThemeKeys::hover, {1.f, 1.f, 1.f, 0.07f});
    theme.set(ThemeKeys::pressed, {1.f, 1.f, 1.f, 0.14f});

    theme.set(ThemeKeys::font, {defaultUIFontFamily(), 13.f});

    theme.set(ThemeKeys::buttonCorner, 5.f);
    theme.set(ThemeKeys::checkboxCorner, 4.f);
    theme.set(ThemeKeys::comboBoxCorner, 5.f);
    theme.set(ThemeKeys::sliderCorner, 4.f);
    theme.set(ThemeKeys::tabCorner, 5.f);
    theme.set(ThemeKeys::textEditorCorner, 4.f);

    return theme;
}
} // namespace

const Theme& defaultTheme()
{
    static const auto theme = makeDefaultTheme();
    return theme;
}
} // namespace eacp::UI
