#include "Tiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GPUComponentsDemo
{
namespace
{
constexpr auto pi = 3.14159265f;
constexpr int particleCount = 6000;
constexpr auto particleBytes = (int) sizeof(Particle) * particleCount;
constexpr int maxRings = 12;

Vector<TileFrame*>& frames()
{
    static auto list = Vector<TileFrame*> {};
    return list;
}

struct TorusMesh
{
    Vector<MeshVertex> vertices;
    Vector<std::uint32_t> indices;
};

TorusMesh buildTorus(float major, float minor, int rings, int sides)
{
    auto mesh = TorusMesh {};

    for (auto ring = 0; ring <= rings; ++ring)
    {
        auto u = (float) ring / (float) rings * 2.f * pi;

        for (auto side = 0; side <= sides; ++side)
        {
            auto v = (float) side / (float) sides * 2.f * pi;
            auto normal = Maths::Vec3 {
                std::cos(u) * std::cos(v), std::sin(v), std::sin(u) * std::cos(v)};
            auto centre =
                Maths::Vec3 {std::cos(u) * major, 0.f, std::sin(u) * major};

            mesh.vertices.add(MeshVertex {centre + normal * minor, normal});
        }
    }

    auto row = (std::uint32_t) (sides + 1);

    for (auto ring = 0; ring < rings; ++ring)
        for (auto side = 0; side < sides; ++side)
        {
            auto a = (std::uint32_t) ring * row + (std::uint32_t) side;
            auto b = a + row;

            for (auto index: {a, b, b + 1, a, b + 1, a + 1})
                mesh.indices.add(index);
        }

    return mesh;
}

Vector<Particle> initialParticles()
{
    auto particles = Vector<Particle> {};
    particles.reserve(particleCount);

    for (auto i = 0; i < particleCount; ++i)
    {
        auto radius = 0.85f * std::sqrt((float) i / (float) particleCount);
        auto angle = 2.39996323f * (float) i;
        auto x = radius * std::cos(angle);
        auto y = radius * std::sin(angle);

        particles.add(Particle {{x, y}, {-y * 0.8f, x * 0.8f}});
    }

    return particles;
}

GPU::Buffer makeParticleBuffer()
{
    auto particles = initialParticles();
    return {GPU::Device::shared(),
            particles.data(),
            particleBytes,
            GPU::BufferUsage::Storage};
}

float aspectOf(UI::Point size)
{
    return size.y > 0.f ? size.x / size.y : 1.f;
}

Maths::Vec3 hue(float phase)
{
    return {0.5f + 0.5f * std::cos(phase),
            0.5f + 0.5f * std::cos(phase - 2.094f),
            0.5f + 0.5f * std::cos(phase + 2.094f)};
}

bool canMultisample()
{
    return GPU::Device::shared().isValid()
           && GPU::Device::shared().supportsSampleCount(4);
}

std::string formatMilliseconds(double ms)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f ms", ms);
    return text;
}

OwningPointer<DemoTile> makeTile(TileKind kind)
{
    static auto serial = 0;
    auto name = std::string {kindName(kind)} + " " + std::to_string(++serial);
    auto tile = OwningPointer<DemoTile> {};

    switch (kind)
    {
        case TileKind::Mesh:
            tile.create<MeshTile>(name);
            break;
        case TileKind::Particles:
            tile.create<ParticlesTile>(name);
            break;
        case TileKind::Msaa:
            tile.create<MsaaTile>(name);
            break;
        case TileKind::Plasma:
            tile.create<PlasmaTile>(name);
            break;
        case TileKind::Feedback:
            tile.create<FeedbackTile>(name);
            break;
        case TileKind::OnDemand:
            tile.create<OnDemandTile>(name);
            break;
    }

    return tile;
}

constexpr int fpsChoices[] = {0, 15, 30, 60};
} // namespace

TileCounters& tileCounters()
{
    static auto counters = TileCounters {};
    return counters;
}

TileSettings& tileSettings()
{
    static auto settings = TileSettings {};
    return settings;
}

const Vector<TileFrame*>& liveTileFrames()
{
    return frames();
}

const char* kindName(TileKind kind)
{
    switch (kind)
    {
        case TileKind::Mesh:
            return "Mesh";
        case TileKind::Particles:
            return "Particles";
        case TileKind::Msaa:
            return "MSAA";
        case TileKind::Plasma:
            return "Plasma";
        case TileKind::Feedback:
            return "Feedback";
        case TileKind::OnDemand:
            return "On demand";
    }

    return "";
}

DemoTile::DemoTile(const std::string& nameToUse)
    : name(nameToUse)
{
}

const std::string& DemoTile::getName() const
{
    return name;
}

bool DemoTile::supportsContinuous() const
{
    return true;
}

void DemoTile::apply(const TileSettings& settings)
{
    if (supportsContinuous())
        setContinuous(settings.continuous);

    setMaxFps(settings.maxFps);
    setGpuTiming(settings.timing);
}

void DemoTile::applyMsaa(bool enabled)
{
    if (canMultisample())
        setSampleCount(enabled ? 4 : 1);
}

void DemoTile::update(Threads::FrameTime time)
{
    auto& counters = tileCounters();
    ++counters.updates;

    if (!isOnScreen())
        ++counters.offScreenUpdates;

    elapsed += time.delta;
    tick(time.delta);
}

void DemoTile::mouseDown(const UI::MouseEvent& event)
{
    if (event.button == eacp::Graphics::MouseButton::Right)
    {
        onContextMenu(*this, event);
        return;
    }

    press(event);
}

void DemoTile::tick(double) {}

void DemoTile::press(const UI::MouseEvent&) {}

void DemoTile::prepareForTarget(GPU::ShaderProgram& shader,
                                GPU::BlendMode blend) const
{
    shader.prepare(sampleCount(),
                   hasDepth(),
                   GPU::PrimitiveTopology::Triangles,
                   blend,
                   colorFormat());
}

UI::Point DemoTile::toClip(UI::Point local, UI::Point size)
{
    auto w = std::max(size.x, 1.f);
    auto h = std::max(size.y, 1.f);

    return {local.x / w * 2.f - 1.f, 1.f - local.y / h * 2.f};
}

MeshTile::MeshTile(const std::string& nameToUse)
    : DemoTile(nameToUse)
{
    auto mesh = buildTorus(0.62f, 0.27f, 48, 24);

    shader.setVertices(mesh.vertices.data(), mesh.vertices.size());
    shader.setIndices(mesh.indices.data(), mesh.indices.size());
    indexCount = mesh.indices.size();

    setDepth(true);
    setWantsKeyboardFocus(true);
    setContinuous(true);
}

std::string_view MeshTile::getKindName() const
{
    return "Mesh";
}

void MeshTile::tick(double seconds)
{
    spin += (float) seconds * 0.6f;

    if (!isOnScreen())
    {
        ++tileCounters().skippedCpuWork;
        shownYaw = targetYaw;
        shownPitch = targetPitch;
        return;
    }

    auto ease = 1.f - std::exp(-(float) seconds * 12.f);
    shownYaw += (targetYaw - shownYaw) * ease;
    shownPitch += (targetPitch - shownPitch) * ease;
}

void MeshTile::targetChanged(GPU::Frame&)
{
    prepareForTarget(shader, GPU::BlendMode::None);
}

void MeshTile::render(GPU::Frame& frame)
{
    auto aspect = aspectOf(frame.logicalSize());
    auto projection = Maths::Mat4::perspective(aspect, 0.8f, 0.1f, 50.f);
    auto view = Maths::Mat4::translation({0.f, 0.f, -distance});

    shader.model =
        Maths::Mat4::rotationX(shownPitch) * Maths::Mat4::rotationY(shownYaw + spin);
    shader.viewProjection = projection * view;
    shader.lightDirection = Maths::Vec3 {0.45f, 0.75f, 0.55f};
    shader.baseColour = hasKeyboardFocus() ? Maths::Vec3 {0.95f, 0.6f, 0.3f}
                                           : Maths::Vec3 {0.35f, 0.62f, 0.95f};

    auto pass = frame.beginPass({{0.07f, 0.08f, 0.11f, 1.f}});
    pass.draw(shader);
}

void MeshTile::press(const UI::MouseEvent&)
{
    grabKeyboardFocus();
    repaint();
}

void MeshTile::mouseDrag(const UI::MouseEvent& event)
{
    auto moved = event.rawDelta;

    if (moved.x == 0.f && moved.y == 0.f)
        moved = event.delta;

    orbit(moved.x * 0.01f, moved.y * 0.01f);
}

bool MeshTile::mouseWheelMove(const UI::MouseEvent& event)
{
    if (!event.modifiers.command && !event.modifiers.control)
        return false;

    auto step =
        event.preciseWheel ? event.wheelDelta.y * 0.01f : event.wheelDelta.y * 0.1f;
    distance = std::clamp(distance * std::exp(-step), 1.8f, 9.f);
    repaint();

    return true;
}

bool MeshTile::keyDown(const UI::KeyEvent& event)
{
    constexpr auto step = 0.15f;

    switch (event.keyCode)
    {
        case UI::KeyCode::LeftArrow:
            orbit(-step, 0.f);
            return true;
        case UI::KeyCode::RightArrow:
            orbit(step, 0.f);
            return true;
        case UI::KeyCode::UpArrow:
            orbit(0.f, -step);
            return true;
        case UI::KeyCode::DownArrow:
            orbit(0.f, step);
            return true;
        default:
            return false;
    }
}

void MeshTile::orbit(float yaw, float pitch)
{
    targetYaw += yaw;
    targetPitch = std::clamp(targetPitch + pitch, -1.4f, 1.4f);

    if (!isContinuous())
    {
        shownYaw = targetYaw;
        shownPitch = targetPitch;
    }

    repaint();
}

ParticlesTile::ParticlesTile(const std::string& nameToUse)
    : DemoTile(nameToUse)
    , bufferA(makeParticleBuffer())
    , bufferB(
          GPU::Device::shared(), nullptr, particleBytes, GPU::BufferUsage::Storage)
{
    integrator.prepare();
    draw.setVertices(fullQuad);
    draw.setInstanceBuffer(1, bufferB, particleCount);

    setContinuous(true);
}

std::string_view ParticlesTile::getKindName() const
{
    return "Particles";
}

void ParticlesTile::tick(double seconds)
{
    step = std::min((float) seconds, 1.f / 30.f);
    pullBoost = std::max(0.f, pullBoost - (float) seconds * 2.f);
    attractor = {0.55f * std::cos((float) elapsed * 0.7f),
                 0.45f * std::sin((float) elapsed * 1.1f)};
}

void ParticlesTile::press(const UI::MouseEvent&)
{
    pullBoost = 3.f;
    repaint();
}

void ParticlesTile::targetChanged(GPU::Frame&)
{
    prepareForTarget(draw, GPU::BlendMode::Additive);
}

void ParticlesTile::render(GPU::Frame& frame)
{
    auto& source = front ? bufferA : bufferB;
    auto& target = front ? bufferB : bufferA;

    {
        auto pass = frame.beginCompute();

        integrator.state = source;
        integrator.next = target;
        integrator.attractorX = attractor.x;
        integrator.attractorY = attractor.y;
        integrator.pull = 6.f + pullBoost * 6.f;
        integrator.damping = 0.998f;
        integrator.timeStep = step;

        pass.dispatch(integrator, particleCount);
    }

    auto pass = frame.beginPass({{0.02f, 0.02f, 0.05f, 1.f}});

    draw.setInstanceBuffer(1, target, particleCount);
    draw.radius = 0.012f;
    draw.aspect = aspectOf(frame.logicalSize());
    draw.brightness = 0.6f;

    pass.drawInstanced(draw, particleCount);

    front = !front;
}

MsaaTile::MsaaTile(const std::string& nameToUse)
    : DemoTile(nameToUse)
{
    draw.setVertices(fullQuad);
    applyMsaa(true);
    setContinuous(true);
}

std::string_view MsaaTile::getKindName() const
{
    return "MSAA";
}

void MsaaTile::tick(double seconds)
{
    angle += (float) seconds * 0.25f;

    if (!isOnScreen())
    {
        ++tileCounters().skippedCpuWork;
        linesStale = true;
        return;
    }

    rebuildLines();
}

void MsaaTile::press(const UI::MouseEvent&)
{
    applyMsaa(sampleCount() == 1);
    repaint();
}

void MsaaTile::rebuildLines()
{
    constexpr int spokes = 90;
    constexpr int starPoints = 17;
    constexpr int starStep = 7;

    lines.clear();

    auto xScale = 1.f / std::max(aspect, 0.01f);

    for (auto i = 0; i < spokes; ++i)
    {
        auto a = angle + (float) i / (float) spokes * 2.f * pi;
        auto inner = 0.12f;
        auto outer = 0.95f;
        auto c = std::cos(a);
        auto s = std::sin(a);
        auto colour = hue(a);

        lines.add(LineInstance {{c * inner * xScale, s * inner},
                                {c * outer * xScale, s * outer},
                                {colour.x, colour.y, colour.z, 1.f}});
    }

    auto corner = [&](int index)
    {
        auto a = -angle * 2.f + (float) index / (float) starPoints * 2.f * pi;
        return UI::Point {std::cos(a) * 0.7f * xScale, std::sin(a) * 0.7f};
    };

    for (auto i = 0; i < starPoints; ++i)
    {
        auto from = corner(i * starStep);
        auto to = corner((i + 1) * starStep);

        lines.add(
            LineInstance {{from.x, from.y}, {to.x, to.y}, {1.f, 1.f, 1.f, 1.f}});
    }

    linesStale = false;
}

void MsaaTile::targetChanged(GPU::Frame&)
{
    prepareForTarget(draw, GPU::BlendMode::None);
}

void MsaaTile::render(GPU::Frame& frame)
{
    auto size = frame.pixelSize();
    auto newAspect = aspectOf(size);

    if (linesStale || newAspect != aspect)
    {
        aspect = newAspect;
        rebuildLines();
    }

    draw.setInstances(1, lines.data(), lines.size());
    draw.pixelSize = Maths::Vec2 {size.x, size.y};
    draw.halfWidth = 0.45f * frame.backingScale();

    auto pass = frame.beginPass({{0.05f, 0.05f, 0.07f, 1.f}});
    pass.drawInstanced(draw, lines.size());
}

PlasmaTile::PlasmaTile(const std::string& nameToUse)
    : DemoTile(nameToUse)
{
    shader.setVertices(fullQuad);
    setTransparent(true);
    setContinuous(true);
}

std::string_view PlasmaTile::getKindName() const
{
    return "Plasma";
}

void PlasmaTile::targetChanged(GPU::Frame&)
{
    prepareForTarget(shader, GPU::BlendMode::AlphaBlendOntoTransparent);
}

void PlasmaTile::render(GPU::Frame& frame)
{
    shader.time = (float) elapsed;
    shader.aspect = aspectOf(frame.logicalSize());
    shader.strength = 0.92f;

    auto pass = frame.beginPass({{0.f, 0.f, 0.f, 0.f}});
    pass.draw(shader);
}

FeedbackTile::FeedbackTile(const std::string& nameToUse)
    : DemoTile(nameToUse)
{
    feedback.setVertices(fullQuad);
    splatDraw.setVertices(fullQuad);
    blit.setVertices(fullQuad);

    setWantsKeyboardFocus(true);
    setContinuous(true);
}

std::string_view FeedbackTile::getKindName() const
{
    return "Feedback";
}

void FeedbackTile::requestThumbnail()
{
    thumbnailWanted = true;
    repaint();
}

bool FeedbackTile::hasThumbnail() const
{
    return thumbnail.has_value();
}

bool FeedbackTile::keyDown(const UI::KeyEvent& event)
{
    if (event.keyCode == UI::KeyCode::Space
        || event.charactersIgnoringModifiers == "c")
    {
        requestThumbnail();
        return true;
    }

    return false;
}

void FeedbackTile::tick(double seconds)
{
    for (auto& burst: bursts)
        burst.colour[3] -= (float) seconds * 1.5f;

    bursts.eraseIf([](const Splat& burst) { return burst.colour[3] <= 0.f; });

    if (!isOnScreen())
    {
        ++tileCounters().skippedCpuWork;
        splatsStale = true;
        return;
    }

    moveEmitters();
}

void FeedbackTile::moveEmitters()
{
    constexpr int emitters = 7;

    splats.clear();

    auto t = (float) elapsed;

    for (auto i = 0; i < emitters; ++i)
    {
        auto phase = (float) i / (float) emitters * 2.f * pi;
        auto x = 0.65f * std::sin(t * (0.6f + 0.13f * (float) i) + phase);
        auto y = 0.6f * std::cos(t * (0.45f + 0.07f * (float) i) + phase * 1.7f);
        auto colour = hue(phase + t * 0.3f);

        splats.add(
            Splat {{x, y}, {0.05f, 0.f}, {colour.x, colour.y, colour.z, 0.5f}});
    }

    for (const auto& burst: bursts)
        splats.add(burst);

    splatsStale = false;
}

void FeedbackTile::press(const UI::MouseEvent& event)
{
    grabKeyboardFocus();

    auto at = toClip(event.position, {getWidth(), getHeight()});
    bursts.add(Splat {{at.x, at.y}, {0.16f, 0.55f}, {1.f, 1.f, 1.f, 1.f}});

    if (event.clickCount > 1)
        requestThumbnail();

    repaint();
}

void FeedbackTile::ensureTargets(GPU::Frame& frame)
{
    auto size = frame.pixelSize();
    auto width = (int) size.x;
    auto height = (int) size.y;
    auto format = frame.drawableFormat();

    auto sized = [&](const std::optional<GPU::Texture>& texture)
    {
        return texture.has_value() && texture->width() == width
               && texture->height() == height && texture->format() == format;
    };

    if (sized(targetA) && sized(targetB))
        return;

    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = format;
    descriptor.renderTarget = true;

    targetA.emplace(GPU::Device::shared().makeTexture(descriptor));
    targetB.emplace(GPU::Device::shared().makeTexture(descriptor));
    targetsCleared = false;
}

void FeedbackTile::targetChanged(GPU::Frame& frame)
{
    auto format = GPU::pixelFormatFor(frame.drawableFormat());

    feedback.prepare(
        1, false, GPU::PrimitiveTopology::Triangles, GPU::BlendMode::None, format);
    splatDraw.prepare(1,
                      false,
                      GPU::PrimitiveTopology::Triangles,
                      GPU::BlendMode::Additive,
                      format);
    prepareForTarget(blit, GPU::BlendMode::None);

    targetA.reset();
    targetB.reset();
}

void FeedbackTile::drawFeedback(GPU::Frame& frame)
{
    auto& previous = front ? *targetA : *targetB;
    auto& next = front ? *targetB : *targetA;

    if (!targetsCleared)
    {
        auto clearing = frame.beginPass(previous, {UI::Color::black()});
        targetsCleared = true;
    }

    if (splatsStale)
        moveEmitters();

    auto pass = frame.beginPass(next, {UI::Color::black()});

    feedback.previous = previous;
    feedback.zoom = 0.985f;
    feedback.twist = 0.006f;
    feedback.decay = 0.97f;
    pass.draw(feedback);

    splatDraw.setInstances(1, splats.data(), splats.size());
    splatDraw.aspect = aspectOf(frame.logicalSize());
    pass.drawInstanced(splatDraw, splats.size());
}

void FeedbackTile::present(GPU::Frame& frame)
{
    auto& next = front ? *targetB : *targetA;
    auto pass = frame.beginPass({UI::Color::black()});

    blit.image = next;
    blit.rect = Maths::Vec4 {-1.f, -1.f, 2.f, 2.f};
    blit.tint = 1.f;
    pass.draw(blit);

    if (thumbnail.has_value())
    {
        blit.image = *thumbnail;
        blit.rect = Maths::Vec4 {0.4f, 0.4f, 0.55f, 0.55f};
        blit.tint = 0.9f;
        pass.draw(blit);
    }
}

void FeedbackTile::captureThumbnail(GPU::Frame& frame)
{
    auto size = frame.pixelSize();
    auto format = frame.drawableFormat();

    if (!thumbnail.has_value() || thumbnail->width() != (int) size.x
        || thumbnail->height() != (int) size.y || thumbnail->format() != format)
    {
        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = (int) size.x;
        descriptor.height = (int) size.y;
        descriptor.format = format;

        thumbnail.emplace(GPU::Device::shared().makeTexture(descriptor));
    }

    frame.copyDrawable(*thumbnail);
    thumbnailWanted = false;
}

void FeedbackTile::render(GPU::Frame& frame)
{
    ensureTargets(frame);
    drawFeedback(frame);
    present(frame);

    if (thumbnailWanted)
        captureThumbnail(frame);

    front = !front;
}

OnDemandTile::OnDemandTile(const std::string& nameToUse)
    : DemoTile(nameToUse)
{
    background.setVertices(fullQuad);
    rings.setVertices(fullQuad);
    setContinuous(false);
}

std::string_view OnDemandTile::getKindName() const
{
    return "On demand";
}

bool OnDemandTile::supportsContinuous() const
{
    return false;
}

void OnDemandTile::placeRing(UI::Point local, bool replaceLast)
{
    auto at = toClip(local, {getWidth(), getHeight()});
    auto colour = hue((float) placed.size() * 0.9f);
    auto ring =
        Splat {{at.x, at.y}, {0.2f, 0.6f}, {colour.x, colour.y, colour.z, 1.f}};

    if (replaceLast && !placed.empty())
        placed.back() = ring;
    else
    {
        if (placed.size() >= maxRings)
            placed.erase(placed.begin());

        placed.add(ring);
    }

    repaint();
}

void OnDemandTile::press(const UI::MouseEvent& event)
{
    placeRing(event.position, false);
}

void OnDemandTile::mouseDrag(const UI::MouseEvent& event)
{
    placeRing(event.position, true);
}

void OnDemandTile::targetChanged(GPU::Frame&)
{
    prepareForTarget(background, GPU::BlendMode::None);
    prepareForTarget(rings, GPU::BlendMode::AlphaBlend);
}

void OnDemandTile::render(GPU::Frame& frame)
{
    auto pass = frame.beginPass({UI::Color::black()});

    background.count = (float) getRenderCount();
    background.background = Maths::Vec3 {0.13f, 0.12f, 0.17f};
    background.highlight = Maths::Vec3 {0.55f, 0.45f, 0.95f};
    pass.draw(background);

    if (placed.empty())
        return;

    rings.setInstances(1, placed.data(), placed.size());
    rings.aspect = aspectOf(frame.logicalSize());
    pass.drawInstanced(rings, placed.size());
}

void TileFrame::Strip::paint(UI::Graphics& g)
{
    g.setColour({0.f, 0.f, 0.f, 0.45f});
    g.fillRect(getLocalBounds());
}

TileFrame::TileFrame(TileKind kindToUse)
    : kind(kindToUse)
    , tile(makeTile(kindToUse))
{
    frames().add(this);

    for (auto choice: fpsChoices)
        fps.addItem(choice == 0 ? "no cap" : std::to_string(choice) + " fps");

    title.setText(tile->getName());
    title.setFontSize(11.f);
    stats.setFontSize(11.f);
    stats.setThemeColour(UI::ThemeKeys::dimText);

    auto& settings = tileSettings();
    tile->apply(settings);

    if (settings.msaa)
        tile->applyMsaa(true);

    auto onContinuous = [this](bool on)
    {
        tile->setContinuous(on);
        tile->repaint();
    };

    auto onFps = [this](int index)
    {
        if (index >= 0)
            tile->setMaxFps(fpsChoices[index]);
    };

    auto onCloseClicked = [this] { onClose(*this); };
    auto onMoveClicked = [this] { onMove(*this); };
    auto onContext = [this](DemoTile&, const UI::MouseEvent& event)
    { showMenu(event); };

    continuous.onChange = onContinuous;
    fps.onChange = onFps;
    close.onClick = onCloseClicked;
    move.onClick = onMoveClicked;
    tile->onContextMenu = onContext;

    addAndMakeVisible(*tile);
    addAndMakeVisible(header);
    addAndMakeVisible(footer);
    header.addChildren({continuous, fps, move, close});
    footer.addChildren({title, stats});

    syncControls();
}

TileFrame::~TileFrame()
{
    menu.dismiss();
    frames().removeAllMatches(this);
}

DemoTile& TileFrame::getTile()
{
    return *tile;
}

TileKind TileFrame::getKind() const
{
    return kind;
}

void TileFrame::syncControls()
{
    continuous.setVisible(tile->supportsContinuous());
    continuous.setChecked(tile->isContinuous());

    auto choice = 0;

    for (auto i = 0; i < (int) std::size(fpsChoices); ++i)
        if (fpsChoices[i] == tile->maxFps())
            choice = i;

    fps.setSelectedIndex(choice);
}

std::string TileFrame::statsText()
{
    auto text = std::to_string(tile->getRenderCount()) + " renders";

    if (tile->sampleCount() > 1)
        text += "  x" + std::to_string(tile->sampleCount());

    if (tile->wasHiddenLastFrame())
        text += "  hidden";
    else if (tile->wasCulledLastFrame())
        text += "  culled";

    if (tile->isGpuTiming())
        text += "  " + formatMilliseconds(tile->lastGpuMilliseconds());

    return text;
}

void TileFrame::refreshStats()
{
    auto text = statsText();

    if (text != stats.getText())
        stats.setText(text);
}

void TileFrame::resized()
{
    auto area = getLocalBounds();
    tile->setBounds(area);

    auto top = area.removeFromTop(26.f);
    header.setBounds(top);

    auto row = header.getLocalBounds().inset(3.f, 2.f);
    close.setBounds(row.removeFromRight(22.f));
    row.removeFromRight(4.f);
    move.setBounds(row.removeFromRight(46.f));
    row.removeFromRight(4.f);
    continuous.setBounds(row.removeFromLeft(54.f));
    row.removeFromLeft(4.f);
    fps.setBounds(row.removeFromLeft(std::min(84.f, row.w)));

    footer.setBounds(area.removeFromBottom(20.f));

    auto line = footer.getLocalBounds().inset(6.f, 0.f);
    title.setBounds(line.removeFromLeft(std::min(90.f, line.w * 0.45f)));
    stats.setBounds(line);
}

void TileFrame::paint(UI::Graphics& g)
{
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
    g.fillRect(getLocalBounds());
}

void TileFrame::paintOverChildren(UI::Graphics& g)
{
    auto focused = tile->hasKeyboardFocus();

    g.setColour(UI::defaultTheme().get(focused ? UI::ThemeKeys::accent
                                               : UI::ThemeKeys::outline));
    g.drawRect(getLocalBounds(), focused ? 2.f : 1.f);
}

void TileFrame::showMenu(const UI::MouseEvent& event)
{
    menu.clear();

    auto toggleContinuous = [this]
    {
        tile->setContinuous(!tile->isContinuous());
        tile->repaint();
        syncControls();
    };

    auto toggleMsaa = [this]
    {
        tile->applyMsaa(tile->sampleCount() == 1);
        tile->repaint();
    };

    auto repaintTile = [this] { tile->repaint(); };
    auto closeTile = [this] { onClose(*this); };
    auto moveTile = [this] { onMove(*this); };

    menu.addSectionHeader(tile->getName());
    menu.addItem("Continuous",
                 toggleContinuous,
                 tile->supportsContinuous(),
                 tile->isContinuous());
    menu.addItem("MSAA x4", toggleMsaa, canMultisample(), tile->sampleCount() > 1);

    auto rates = UI::PopupMenu {};

    for (auto choice: fpsChoices)
    {
        auto cap = [this, choice]
        {
            tile->setMaxFps(choice);
            syncControls();
        };

        rates.addItem(choice == 0 ? "No cap" : std::to_string(choice) + " fps",
                      cap,
                      true,
                      tile->maxFps() == choice);
    }

    menu.addSubMenu("Max fps", rates);
    menu.addItem("Repaint", repaintTile);
    menu.addSeparator();
    menu.addItem("Move", moveTile);
    menu.addItem("Close", closeTile);

    menu.showAtMouse(*tile, event);
}
} // namespace GPUComponentsDemo
