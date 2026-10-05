#include <eacp/Core/Utils/Strings.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/Text/TextRenderer.h>
#include <eacp/UI/Render/CoverageAtlas.h>
#include <eacp/UI/Render/GradientRamps.h>
#include <eacp/UI/Render/ShapeBatch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

// Every control of a game controller doing something you can see, through
// GameInput's gamepads. A disc in an arena: the left stick moves it, the right
// stick aims its turret, the right trigger fires as fast as it is pulled, the
// left trigger raises a shield, the bumpers recolour it, the face buttons
// jump, dash, drop a marker and clear, the D-pad nudges it, the stick clicks
// toggle running and recentre, Start pauses, Back hides the panel and Home
// flashes. Every press and release announces itself in the log, and the
// panel shows all fifteen buttons and six axes as the frame sees them, named
// as the controller's family prints them.

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto background = Color {0.08f, 0.09f, 0.12f};
constexpr auto panelColor = Color {1.f, 1.f, 1.f, 0.07f};
constexpr auto labelColor = Color::gray(0.92f);
constexpr auto dimColor = Color::gray(0.5f);
constexpr auto panelWidth = 320.f;
constexpr auto margin = 20.f;
constexpr auto playerRadius = 22.f;
constexpr auto walkSpeed = 320.f;
constexpr auto runFactor = 1.8f;
constexpr auto dashSpeed = 1400.f;
constexpr auto dashDrag = 6.f;
constexpr auto jumpSpeed = 520.f;
constexpr auto gravity = 1500.f;
constexpr auto nudge = 40.f;
constexpr auto bulletSpeed = 700.f;
constexpr auto bulletLife = 1.4f;
constexpr auto shotsPerSecond = 18.f;
constexpr auto deadZone = 0.15f;
constexpr auto announcementLife = 4.0;
constexpr auto maxAnnouncements = 9;
constexpr auto maxMarkers = 40;

constexpr auto palette = std::to_array<Color>({
    {0.95f, 0.36f, 0.42f},
    {0.36f, 0.72f, 0.98f},
    {0.52f, 0.86f, 0.46f},
    {0.98f, 0.76f, 0.30f},
    {0.74f, 0.52f, 0.98f},
    {0.30f, 0.88f, 0.82f},
    {0.98f, 0.56f, 0.24f},
    {0.94f, 0.50f, 0.82f},
});

Color colorFor(int index)
{
    constexpr auto count = (int) palette.size();
    return palette[((index % count) + count) % count];
}

Point scaled(Point point, float factor)
{
    return {point.x * factor, point.y * factor};
}

// Sticks arrive raw, and a resting one rarely reads zero: inside the dead zone
// is nothing, outside it is rescaled to start from zero.
Point outsideDeadZone(Point stick)
{
    const auto length = stick.length();

    if (length < deadZone)
        return {};

    const auto scale = std::min((length - deadZone) / (1.f - deadZone), 1.f);
    return scaled(stick.normalized(), scale);
}

// Up on a stick is down the screen's y.
Point toScreen(Point stick)
{
    return {stick.x, -stick.y};
}

Rect discRect(Point centre, float radius)
{
    return {centre.x - radius, centre.y - radius, radius * 2.f, radius * 2.f};
}

// What the controller prints on the face buttons, by family; the rest are
// named the same everywhere.
std::string_view buttonName(GamepadButton button, GamepadFamily family)
{
    using Button = GamepadButton;
    using Family = GamepadFamily;

    switch (button)
    {
        case Button::South:
            return family == Family::Xbox          ? "A"
                   : family == Family::PlayStation ? "Cross"
                   : family == Family::Nintendo    ? "B"
                                                   : "South";
        case Button::East:
            return family == Family::Xbox          ? "B"
                   : family == Family::PlayStation ? "Circle"
                   : family == Family::Nintendo    ? "A"
                                                   : "East";
        case Button::West:
            return family == Family::Xbox          ? "X"
                   : family == Family::PlayStation ? "Square"
                   : family == Family::Nintendo    ? "Y"
                                                   : "West";
        case Button::North:
            return family == Family::Xbox          ? "Y"
                   : family == Family::PlayStation ? "Triangle"
                   : family == Family::Nintendo    ? "X"
                                                   : "North";
        case Button::LeftShoulder:
            return "LB";
        case Button::RightShoulder:
            return "RB";
        case Button::LeftStick:
            return "L stick click";
        case Button::RightStick:
            return "R stick click";
        case Button::Start:
            return "Start";
        case Button::Back:
            return "Back";
        case Button::Home:
            return "Home";
        case Button::DpadUp:
            return "D-pad up";
        case Button::DpadDown:
            return "D-pad down";
        case Button::DpadLeft:
            return "D-pad left";
        case Button::DpadRight:
            return "D-pad right";
        default:
            return "?";
    }
}

std::string_view axisName(GamepadAxis axis)
{
    switch (axis)
    {
        case GamepadAxis::LeftX:
            return "Left X";
        case GamepadAxis::LeftY:
            return "Left Y";
        case GamepadAxis::RightX:
            return "Right X";
        case GamepadAxis::RightY:
            return "Right Y";
        case GamepadAxis::LeftTrigger:
            return "LT";
        case GamepadAxis::RightTrigger:
            return "RT";
        default:
            return "?";
    }
}

std::string_view familyName(GamepadFamily family)
{
    switch (family)
    {
        case GamepadFamily::Xbox:
            return "Xbox";
        case GamepadFamily::PlayStation:
            return "PlayStation";
        case GamepadFamily::Nintendo:
            return "Nintendo";
        default:
            return "Generic";
    }
}

bool isTrigger(GamepadAxis axis)
{
    return axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
}

struct Bullet final
{
    Point pos;
    Point velocity;
    float life = bulletLife;
    Color color;
};

struct Marker final
{
    Point pos;
    Color color;
};

struct Announcement final
{
    std::string text;
    double time = 0.0;
};

struct GamepadView final : GPU::GPUView
{
    GamepadView()
    {
        setSampleCount(1);
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        if (input == nullptr)
            return;

        const auto& frame = input->snapshot();
        const auto delta = (float) std::clamp(time.delta, 0.0, 0.1);
        now = frame.time();

        for (const auto& event: frame.events())
            announceEvent(frame, event);

        // Kept for render, which must not take a snapshot of its own: a second
        // drain in the same frame would eat the edges the next update expects.
        padCount = frame.gamepads().size();
        firstPad = padCount > 0 ? std::optional {frame.gamepads()[0]} : std::nullopt;
        backendName = input->backendName();

        if (firstPad)
            steer(*firstPad, delta);
        else
            shield = 0.f;

        if (!paused)
            simulate(delta);

        flash = std::max(flash - delta * 3.f, 0.f);
    }

    void announceEvent(const GameInputFrame& frame, const InputEvent& event)
    {
        if (!event.isGamepad())
            return;

        const auto family = familyOf(frame, event.gamepad);
        const auto button = buttonName((GamepadButton) event.code, family);

        switch (event.type)
        {
            case InputEventType::GamepadConnected:
                announce(Strings::concat(familyName((GamepadFamily) event.code),
                                         " controller ",
                                         event.gamepad,
                                         " connected"));
                return;
            case InputEventType::GamepadDisconnected:
                announce(
                    Strings::concat("controller ", event.gamepad, " disconnected"));
                return;
            case InputEventType::GamepadDown:
                announce(Strings::concat(button, " pressed"));
                return;
            case InputEventType::GamepadUp:
                announce(Strings::concat(button, " released"));
                return;
            default:
                return;
        }
    }

    static GamepadFamily familyOf(const GameInputFrame& frame, int id)
    {
        for (const auto& pad: frame.gamepads())
            if (pad.id() == id)
                return pad.family();

        return GamepadFamily::Generic;
    }

    void announce(std::string message)
    {
        announcements.add({std::move(message), now});

        while (announcements.size() > maxAnnouncements)
            announcements.removeAt(0);
    }

    // The first pad is the player's.
    void steer(const GamepadState& pad, float delta)
    {
        using Button = GamepadButton;
        using Axis = GamepadAxis;

        const auto move = toScreen(outsideDeadZone(pad.leftStick()));
        const auto look = toScreen(outsideDeadZone(pad.rightStick()));

        if (look.length() > 0.f)
            aim = look;

        if (pad.wasPressed(Button::South) && height <= 0.f)
            verticalSpeed = jumpSpeed;

        if (pad.wasPressed(Button::East))
            dash =
                scaled((move.length() > 0.f ? move : aim).normalized(), dashSpeed);

        if (pad.wasPressed(Button::West))
            dropMarker();

        if (pad.wasPressed(Button::North))
        {
            markers.clear();
            bullets.clear();
        }

        if (pad.wasPressed(Button::LeftShoulder))
            --colorIndex;

        if (pad.wasPressed(Button::RightShoulder))
            ++colorIndex;

        if (pad.wasPressed(Button::LeftStick))
            running = !running;

        if (pad.wasPressed(Button::RightStick))
            pos = arenaCentre();

        if (pad.wasPressed(Button::Start))
            paused = !paused;

        if (pad.wasPressed(Button::Back))
            showPanel = !showPanel;

        if (pad.wasPressed(Button::Home))
            flash = 1.f;

        if (pad.wasPressed(Button::DpadUp))
            pos.y -= nudge;

        if (pad.wasPressed(Button::DpadDown))
            pos.y += nudge;

        if (pad.wasPressed(Button::DpadLeft))
            pos.x -= nudge;

        if (pad.wasPressed(Button::DpadRight))
            pos.x += nudge;

        shield = pad.axis(Axis::LeftTrigger);

        if (paused)
            return;

        const auto speed = walkSpeed * (running ? runFactor : 1.f);
        pos = pos + scaled(move, speed * delta);

        fire(pad.axis(Axis::RightTrigger), delta);
    }

    // The trigger is a rate: a light pull is a slow stream, a full one is
    // shotsPerSecond.
    void fire(float trigger, float delta)
    {
        if (trigger <= 0.f)
        {
            shotsOwed = 0.f;
            return;
        }

        shotsOwed += trigger * shotsPerSecond * delta;

        while (shotsOwed >= 1.f)
        {
            shotsOwed -= 1.f;
            bullets.add(
                {pos, scaled(aim.normalized(), bulletSpeed), bulletLife, color()});
        }
    }

    void dropMarker()
    {
        markers.add({pos, color()});

        while (markers.size() > maxMarkers)
            markers.removeAt(0);
    }

    void simulate(float delta)
    {
        pos = pos + scaled(dash, delta);
        dash = scaled(dash, std::max(1.f - dashDrag * delta, 0.f));

        verticalSpeed -= gravity * delta;
        height = std::max(height + verticalSpeed * delta, 0.f);

        if (height <= 0.f)
            verticalSpeed = 0.f;

        const auto arena = arenaRect();
        pos.x =
            std::clamp(pos.x, arena.x + playerRadius, arena.right() - playerRadius);
        pos.y =
            std::clamp(pos.y, arena.y + playerRadius, arena.bottom() - playerRadius);

        for (auto& bullet: bullets)
        {
            bullet.pos = bullet.pos + scaled(bullet.velocity, delta);
            bullet.life -= delta;
        }

        bullets.eraseIf([](const Bullet& bullet) { return bullet.life <= 0.f; });
    }

    Color color() const { return colorFor(colorIndex); }

    Rect arenaRect() const
    {
        auto bounds = getLocalBounds();
        auto width =
            bounds.w - margin * 2.f - (showPanel ? panelWidth + margin : 0.f);

        return {margin,
                margin,
                std::max(width, 1.f),
                std::max(bounds.h - margin * 2.f, 1.f)};
    }

    Point arenaCentre() const
    {
        auto arena = arenaRect();
        return {arena.x + arena.w * 0.5f, arena.y + arena.h * 0.5f};
    }

    void resized() override
    {
        if (!placed)
        {
            pos = arenaCentre();
            placed = true;
        }
    }

    void render(GPU::Frame& frame) override
    {
        auto size = frame.logicalSize();
        auto scale = frame.backingScale();
        auto pass = frame.beginPass({background});

        if (size.x <= 0.f || size.y <= 0.f)
            return;

        if (!shapes)
            shapes.emplace(atlas, ramps, size, scale, sampleCount());

        shapes->setLogicalSize(size);
        shapes->setPixelScale(scale);
        shapes->begin(pass);

        text.setViewport(size, scale);
        text.begin();

        drawArena();
        drawAnnouncements();

        if (showPanel)
            drawPanel();

        if (flash > 0.f)
            shapes->fillRect({0.f, 0.f, size.x, size.y}, Color::white(flash * 0.6f));

        shapes->end();
        text.flush(pass);
    }

    void drawArena()
    {
        const auto arena = arenaRect();
        shapes->drawRect(arena, Color::gray(0.3f), 2.f, 12.f);

        for (const auto& marker: markers)
            shapes->fillRect(
                discRect(marker.pos, 7.f), marker.color.withAlpha(0.8f), 2.f);

        for (const auto& bullet: bullets)
            shapes->fillRect(discRect(bullet.pos, 5.f),
                             bullet.color.withAlpha(bullet.life / bulletLife),
                             5.f);

        drawPlayer();
        drawTitle(arena);

        if (paused)
            drawCentred("PAUSED", arenaCentre(), 40.f, labelColor);
    }

    void drawPlayer()
    {
        const auto tint = color();
        const auto lift = height * 0.35f;
        const auto shadowRadius =
            playerRadius * (1.f - std::min(lift / 200.f, 0.4f));
        const auto body = Point {pos.x, pos.y - lift};

        shapes->fillRect(
            discRect(pos, shadowRadius), Color::black(0.45f), shadowRadius);

        if (shield > 0.f)
        {
            const auto radius = playerRadius + 10.f + shield * 40.f;
            shapes->drawRect(discRect(body, radius),
                             tint.withAlpha(0.35f + shield * 0.5f),
                             3.f,
                             radius);
        }

        const auto turret =
            body
            + scaled(aim.normalized(), playerRadius + 14.f + aim.length() * 30.f);
        shapes->drawLine(body, turret, tint, 6.f);

        shapes->fillRect(discRect(body, playerRadius), tint, playerRadius);
        shapes->drawRect(discRect(body, playerRadius),
                         Color::white(running ? 0.95f : 0.4f),
                         running ? 4.f : 2.f,
                         playerRadius);
    }

    void drawTitle(const Rect& arena)
    {
        auto left = arena.x + 16.f;
        auto top = arena.y + 14.f + text.ascent();

        text.draw("Gamepad Demo", {left, top}, labelColor);
        top += text.lineHeight() + 2.f;

        const auto lines = std::array<std::string_view, 3> {
            "Left stick moves, right stick aims, RT fires, LT shields, LB/RB recolour",
            "A jumps, B dashes, X drops a marker, Y clears, D-pad nudges, stick clicks run / recentre",
            "Start pauses, Back hides the panel, Home flashes"};

        for (auto line: lines)
        {
            text.draw(line, {left, top}, dimColor);
            top += text.lineHeight();
        }
    }

    void drawAnnouncements()
    {
        const auto arena = arenaRect();
        auto bottom = arena.bottom() - 14.f;

        for (auto index = announcements.size() - 1; index >= 0; --index)
        {
            const auto& entry = announcements[index];
            const auto age = now - entry.time;
            const auto alpha = (float) std::clamp(
                1.0 - (age - 1.0) / (announcementLife - 1.0), 0.0, 1.0);

            if (alpha <= 0.f)
                continue;

            text.draw(
                entry.text, {arena.x + 16.f, bottom}, labelColor.withAlpha(alpha));
            bottom -= text.lineHeight();
        }
    }

    void drawPanel()
    {
        const auto bounds = getLocalBounds();
        const auto panel = Rect {bounds.w - margin - panelWidth,
                                 margin,
                                 panelWidth,
                                 bounds.h - margin * 2.f};
        shapes->fillRect(panel, panelColor, 12.f);

        auto left = panel.x + 16.f;
        auto top = panel.y + 14.f + text.ascent();

        if (!firstPad)
        {
            text.draw("No controller", {left, top}, labelColor);
            top += text.lineHeight();
            text.draw("Plug one in: it is picked up live", {left, top}, dimColor);
            top += text.lineHeight();
            text.draw(
                Strings::concat("input: ", backendName), {left, top}, dimColor);
            return;
        }

        text.draw(Strings::concat(familyName(firstPad->family()),
                                  " controller ",
                                  firstPad->id(),
                                  ", player ",
                                  firstPad->playerIndex() + 1),
                  {left, top},
                  labelColor);
        top += text.lineHeight();
        text.draw(Strings::concat(padCount, " connected, input: ", backendName),
                  {left, top},
                  dimColor);
        top += text.lineHeight() + 10.f;

        top = drawButtons(*firstPad, left, top, panel.w - 32.f);
        drawAxes(*firstPad, left, top + 10.f, panel.w - 32.f);
    }

    float drawButtons(const GamepadState& pad, float left, float top, float width)
    {
        constexpr auto columns = 3;
        const auto gap = 6.f;
        const auto chipWidth = (width - gap * (columns - 1)) / columns;
        const auto chipHeight = text.lineHeight() + 10.f;
        const auto tint = color();

        for (auto index = 0; index < GamepadState::buttonCount; ++index)
        {
            const auto button = (GamepadButton) index;
            const auto column = index % columns;
            const auto row = index / columns;
            const auto chip = Rect {left + column * (chipWidth + gap),
                                    top - text.ascent() + row * (chipHeight + gap),
                                    chipWidth,
                                    chipHeight};
            const auto down = pad.isDown(button);

            if (down)
                shapes->fillRect(chip, tint, 6.f);
            else
                shapes->drawRect(chip, Color::gray(0.35f), 1.f, 6.f);

            text.draw(buttonName(button, pad.family()),
                      {chip.x + 8.f, chip.y + 5.f + text.ascent()},
                      down ? Color::black(0.85f) : dimColor);
        }

        const auto rows = (GamepadState::buttonCount + columns - 1) / columns;
        return top + rows * (chipHeight + gap);
    }

    void drawAxes(const GamepadState& pad, float left, float top, float width)
    {
        const auto labelWidth = 64.f;
        const auto barLeft = left + labelWidth;
        const auto barWidth = width - labelWidth - 50.f;
        const auto barHeight = 10.f;
        const auto tint = color();

        for (auto index = 0; index < GamepadState::axisCount; ++index)
        {
            const auto axis = (GamepadAxis) index;
            const auto value = pad.axis(axis);
            const auto barTop = top - barHeight * 0.5f - text.ascent() * 0.35f;

            text.draw(axisName(axis), {left, top}, dimColor);
            shapes->drawRect({barLeft, barTop, barWidth, barHeight},
                             Color::gray(0.35f),
                             1.f,
                             5.f);

            if (isTrigger(axis))
                shapes->fillRect({barLeft,
                                  barTop,
                                  barWidth * std::clamp(value, 0.f, 1.f),
                                  barHeight},
                                 tint,
                                 5.f);
            else
            {
                const auto centre = barLeft + barWidth * 0.5f;
                const auto extent = barWidth * 0.5f * std::clamp(value, -1.f, 1.f);
                shapes->fillRect({std::min(centre, centre + extent),
                                  barTop,
                                  std::abs(extent),
                                  barHeight},
                                 tint,
                                 5.f);
                shapes->drawLine({centre, barTop - 2.f},
                                 {centre, barTop + barHeight + 2.f},
                                 Color::gray(0.5f),
                                 1.f);
            }

            auto label = std::array<char, 16> {};
            std::snprintf(label.data(), label.size(), "%+.2f", value);
            text.draw(label.data(), {barLeft + barWidth + 8.f, top}, labelColor);

            top += text.lineHeight() + 8.f;
        }
    }

    void drawCentred(std::string_view string,
                     Point centre,
                     float pointSize,
                     const Color& tint)
    {
        auto font = text.getFont();
        font.pointSize = pointSize;

        const auto width = text.measure(string, font);
        text.draw(string,
                  {centre.x - width * 0.5f, centre.y + text.ascent(font) * 0.35f},
                  tint,
                  font);
    }

    Window* window = nullptr;
    GameInput* input = nullptr;

    UI::CoverageAtlas atlas;
    UI::GradientRamps ramps;
    std::optional<UI::ShapeBatch> shapes;
    Text::TextRenderer text {15.f};

    Point pos;
    Point aim {0.f, -1.f};
    Point dash;
    float height = 0.f;
    float verticalSpeed = 0.f;
    float shield = 0.f;
    float shotsOwed = 0.f;
    float flash = 0.f;
    int colorIndex = 1;
    bool running = false;
    bool paused = false;
    bool showPanel = true;
    bool placed = false;
    double now = 0.0;

    std::optional<GamepadState> firstPad;
    int padCount = 0;
    std::string backendName;

    Vector<Bullet> bullets;
    Vector<Marker> markers;
    Vector<Announcement> announcements;
};

struct GamepadApp final
{
    GamepadApp()
    {
        view.window = &window;
        view.input = &input;
        view.focus();
    }

    static WindowOptions options()
    {
        auto result = WindowOptions {};
        result.width = 1180;
        result.height = 720;
        result.title = "Gamepad Demo";
        result.backgroundColor = background;
        return result;
    }

    GamepadView view;
    Window window {view, options()};
    GameInput input {window};
};
} // namespace

int main()
{
    return Apps::run<GamepadApp>();
}
