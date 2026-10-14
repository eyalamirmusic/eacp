#pragma once

#include <eacp/UI/UI.h>

#include <cstdint>
#include <string>

inline bool hasDevice()
{
    return eacp::GPU::Device::shared().isValid();
}

inline const auto red = eacp::Graphics::Color {1.f, 0.f, 0.f, 1.f};
inline const auto blue = eacp::Graphics::Color {0.f, 0.f, 1.f, 1.f};

inline eacp::Graphics::MouseEvent mouseAt(eacp::Graphics::Point position,
                                          int clickCount = 1)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;
    event.clickCount = clickCount;

    return event;
}

inline eacp::Graphics::KeyEvent keyOf(std::uint16_t code,
                                      const std::string& characters = {})
{
    auto event = eacp::Graphics::KeyEvent {};

    event.keyCode = code;
    event.characters = characters;
    event.charactersIgnoringModifiers = characters;

    return event;
}

inline eacp::GPUWidgets::Path rect(const eacp::Graphics::Rect& bounds)
{
    auto path = eacp::GPUWidgets::Path {};
    path.addRect(bounds);

    return path;
}

// A painter with nowhere to draw, which is all a recording needs.
struct Recorder
{
    explicit Recorder(const eacp::Graphics::Rect& bounds = {0.f, 0.f, 100.f, 50.f})
        : g(list, ramps, text, bounds)
    {
    }

    eacp::UI::DrawList list;
    eacp::UI::GradientRamps ramps;
    eacp::Text::TextRenderer text;
    eacp::UI::Graphics g;
};
