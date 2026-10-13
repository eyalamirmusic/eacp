#pragma once

#include "Shaders.h"

#include <eacp/UI/UI.h>

#include <cstdint>
#include <optional>
#include <string>

namespace GPUComponentsDemo
{
using namespace eacp;

// Process-wide tallies every tile's update() adds to. The interesting pair is
// the second against the first: a continuous tile that is scrolled out or on a
// hidden tab is still ticked -- its logic keeps time -- and only its render()
// and whatever CPU work exists purely to feed one are skipped.
struct TileCounters
{
    std::int64_t updates = 0;
    std::int64_t offScreenUpdates = 0;
    std::int64_t skippedCpuWork = 0;
};

TileCounters& tileCounters();

struct TileSettings
{
    bool continuous = true;
    int maxFps = 0;
    bool msaa = false;
    bool timing = false;
};

enum class TileKind
{
    Mesh,
    Particles,
    Msaa,
    Plasma,
    Feedback,
    OnDemand
};

constexpr int tileKindCount = 6;

// Each tile makes its pipelines in targetChanged, which the component calls
// before the first render and again whenever the MSAA toggle or a depth change
// remakes the texture under it.
class DemoTile : public UI::GPUComponent
{
public:
    explicit DemoTile(const std::string& nameToUse);

    const std::string& getName() const;
    virtual std::string_view getKindName() const = 0;
    virtual bool supportsContinuous() const;

    void apply(const TileSettings& settings);
    void applyMsaa(bool enabled);

    void update(Threads::FrameTime time) final;
    void mouseDown(const UI::MouseEvent& event) final;

    std::function<void(DemoTile&, const UI::MouseEvent&)> onContextMenu =
        [](DemoTile&, const UI::MouseEvent&) {};

protected:
    virtual void tick(double seconds);
    virtual void press(const UI::MouseEvent& event);

    void prepareForTarget(GPU::ShaderProgram& shader, GPU::BlendMode blend) const;
    static UI::Point toClip(UI::Point local, UI::Point size);

    double elapsed = 0.0;

private:
    std::string name;
};

class MeshTile final : public DemoTile
{
public:
    explicit MeshTile(const std::string& nameToUse);

    std::string_view getKindName() const override;
    void targetChanged(GPU::Frame& frame) override;
    void render(GPU::Frame& frame) override;

    void mouseDrag(const UI::MouseEvent& event) override;
    bool mouseWheelMove(const UI::MouseEvent& event) override;
    bool keyDown(const UI::KeyEvent& event) override;

private:
    void tick(double seconds) override;
    void press(const UI::MouseEvent& event) override;
    void orbit(float yaw, float pitch);

    MeshShader shader;
    int indexCount = 0;
    float spin = 0.f;
    float targetYaw = 0.4f;
    float targetPitch = 0.5f;
    float shownYaw = 0.4f;
    float shownPitch = 0.5f;
    float distance = 3.2f;
};

class ParticlesTile final : public DemoTile
{
public:
    explicit ParticlesTile(const std::string& nameToUse);

    std::string_view getKindName() const override;
    void targetChanged(GPU::Frame& frame) override;
    void render(GPU::Frame& frame) override;

private:
    void tick(double seconds) override;
    void press(const UI::MouseEvent& event) override;

    GPU::Buffer bufferA;
    GPU::Buffer bufferB;
    IntegrateParticles integrator;
    DrawParticles draw;
    UI::Point attractor;
    float step = 1.f / 60.f;
    float pullBoost = 0.f;
    bool front = true;
};

class MsaaTile final : public DemoTile
{
public:
    explicit MsaaTile(const std::string& nameToUse);

    std::string_view getKindName() const override;
    void targetChanged(GPU::Frame& frame) override;
    void render(GPU::Frame& frame) override;

private:
    void tick(double seconds) override;
    void press(const UI::MouseEvent& event) override;
    void rebuildLines();

    DrawLines draw;
    Vector<LineInstance> lines;
    float angle = 0.f;
    float aspect = 1.f;
    bool linesStale = true;
};

class PlasmaTile final : public DemoTile
{
public:
    explicit PlasmaTile(const std::string& nameToUse);

    std::string_view getKindName() const override;
    void targetChanged(GPU::Frame& frame) override;
    void render(GPU::Frame& frame) override;

private:
    PlasmaShader shader;
};

class FeedbackTile final : public DemoTile
{
public:
    explicit FeedbackTile(const std::string& nameToUse);

    std::string_view getKindName() const override;
    void targetChanged(GPU::Frame& frame) override;
    void render(GPU::Frame& frame) override;
    bool keyDown(const UI::KeyEvent& event) override;

    void requestThumbnail();
    bool hasThumbnail() const;

private:
    void tick(double seconds) override;
    void press(const UI::MouseEvent& event) override;
    void moveEmitters();
    void ensureTargets(GPU::Frame& frame);
    void drawFeedback(GPU::Frame& frame);
    void present(GPU::Frame& frame);
    void captureThumbnail(GPU::Frame& frame);

    FeedbackShader feedback;
    DrawSplats splatDraw;
    BlitShader blit;

    std::optional<GPU::Texture> targetA;
    std::optional<GPU::Texture> targetB;
    std::optional<GPU::Texture> thumbnail;
    Vector<Splat> splats;
    Vector<Splat> bursts;
    bool front = true;
    bool targetsCleared = false;
    bool thumbnailWanted = false;
    bool splatsStale = true;
};

class OnDemandTile final : public DemoTile
{
public:
    explicit OnDemandTile(const std::string& nameToUse);

    std::string_view getKindName() const override;
    bool supportsContinuous() const override;
    void targetChanged(GPU::Frame& frame) override;
    void render(GPU::Frame& frame) override;

    void mouseDrag(const UI::MouseEvent& event) override;

private:
    void press(const UI::MouseEvent& event) override;
    void placeRing(UI::Point local, bool replaceLast);

    CounterShader background;
    DrawSplats rings;
    Vector<Splat> placed;
};

// A tile with its chrome: a header of ordinary widgets drawn over the top of the
// GPU content, and a line of figures over its foot. The frame owns its tile.
class TileFrame final : public UI::Component
{
public:
    explicit TileFrame(TileKind kind);
    ~TileFrame() override;

    DemoTile& getTile();
    TileKind getKind() const;

    void refreshStats();
    void syncControls();

    void resized() override;
    void paint(UI::Graphics& g) override;
    void paintOverChildren(UI::Graphics& g) override;

    std::function<void(TileFrame&)> onClose = [](TileFrame&) {};
    std::function<void(TileFrame&)> onMove = [](TileFrame&) {};

private:
    struct Strip final : UI::Component
    {
        void paint(UI::Graphics& g) override;
    };

    void showMenu(const UI::MouseEvent& event);
    std::string statsText();

    TileKind kind;
    OwningPointer<DemoTile> tile;
    Strip header;
    Strip footer;
    UI::Label title;
    UI::Label stats;
    UI::Button close {"x"};
    UI::Button move {"move"};
    UI::Checkbox continuous {"live"};
    UI::ComboBox fps {"fps"};
    UI::PopupMenu menu;
};

const Vector<TileFrame*>& liveTileFrames();
TileSettings& tileSettings();
const char* kindName(TileKind kind);
} // namespace GPUComponentsDemo
