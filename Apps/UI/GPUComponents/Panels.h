#pragma once

#include "Tiles.h"

#include <random>

namespace GPUComponentsDemo
{
// Owns a set of tile frames and lays them out in rows. Closing a tile and
// moving it out are deferred to the loop, since both arrive from a click on a
// widget inside the frame being taken away.
class TileShelf final : public UI::Component
{
public:
    void setTileSize(UI::Point size);
    float heightFor(float width) const;

    TileFrame& add(TileKind kind);
    void moveTile(TileFrame& frame, TileShelf& destination);
    void remove(TileFrame& frame);
    void clear();

    int getNumTiles() const;
    TileFrame& getTileFrame(int index) const;
    bool holds(const TileFrame* frame) const;

    void resized() override;
    void paint(UI::Graphics& g) override;

    std::function<void(TileFrame&)> onMoveRequested = [](TileFrame&) {};
    std::function<void()> onTilesChanged = [] {};

private:
    void wire(TileFrame& frame);
    int columnsFor(float width) const;
    int indexOf(const TileFrame* frame) const;

    Vector<OwningPointer<TileFrame>> tiles;
    UI::Point tileSize {250.f, 190.f};
    float gap = 8.f;
};

class GridPanel final : public UI::Component
{
public:
    GridPanel();

    void setTileCount(int count);
    void resized() override;
    void paint(UI::Graphics& g) override;

private:
    void layOutShelf();

    UI::Label countCaption {"Tiles"};
    UI::ComboBox count;
    UI::Label widthCaption {"Viewport width"};
    UI::Slider viewportWidth;
    UI::Label hint {"Scroll the grid: tiles outside the viewport are culled and "
                    "stop rendering, but keep updating."};
    UI::Viewport viewport;
    TileShelf shelf;
};

// A component that draws the layer capturing its child, so the child and the
// GPU tile inside it composite as one picture and fade as one.
class FadingGroup final : public UI::Component
{
public:
    FadingGroup();

    void setFade(float opacity);
    void resized() override;
    void paint(UI::Graphics& g) override;

private:
    struct Contents final : UI::Component
    {
        Contents();
        void resized() override;
        void paint(UI::Graphics& g) override;

        UI::Label caption {"Captured group: faded as one"};
        TileFrame tile {TileKind::Particles};
        UI::Slider knobLike;
    };

    Contents contents;
    UI::Layer layer {*this};
};

// A plain widget card laid over GPU tiles, to show ordinary drawing composited
// above a component's texture in tree order.
class OverlayCard final : public UI::Component
{
public:
    OverlayCard();

    void resized() override;
    void paint(UI::Graphics& g) override;

private:
    UI::Label text {"A widget card over two tiles"};
    UI::Button button {"Click me"};
    UI::Label clicks;
    int clickCount = 0;
};

class OverlapPanel final : public UI::Component
{
public:
    OverlapPanel();

    void resized() override;
    void paint(UI::Graphics& g) override;

private:
    void placeTransform();

    UI::Label opacityCaption {"Mesh opacity"};
    UI::Slider opacity;
    UI::Label rotationCaption {"Mesh rotation"};
    UI::Slider rotation;
    UI::Label scaleCaption {"Mesh scale"};
    UI::Slider scale;
    UI::Label fadeCaption {"Group fade"};
    UI::Slider fade;
    UI::Label comboCaption {"A list that opens over the tiles"};
    UI::ComboBox longList {"choose"};
    UI::Label menuHint {"Right-click any tile for its menu"};

    TileFrame mesh {TileKind::Mesh};
    TileFrame plasma {TileKind::Plasma};
    TileFrame feedback {TileKind::Feedback};
    TileFrame onDemand {TileKind::OnDemand};
    OverlayCard card;
    FadingGroup group;
};

class ChurnPanel final : public UI::Component
{
public:
    ChurnPanel();
    ~ChurnPanel() override;

    void resized() override;
    void paint(UI::Graphics& g) override;

private:
    void addTile();
    void removeTile();
    void reparentTile();
    void churnOnce();
    void setChurning(bool shouldChurn);
    void moveToOtherShelf(TileFrame& frame);
    void updateCounts();
    int totalTiles() const;
    TileShelf& shelfHolding(TileFrame& frame);
    TileShelf& randomShelf();

    UI::Button addButton {"Add"};
    UI::Button removeButton {"Remove"};
    UI::Button reparentButton {"Reparent"};
    UI::Checkbox churn {"Random churn (seeded)"};
    UI::Label counts;
    UI::Label leftCaption {"Shelf A"};
    UI::Label rightCaption {"Shelf B"};
    TileShelf left;
    TileShelf right;

    std::mt19937 random {20261013u};
    OwningPointer<Threads::Timer> churnTimer;
    int added = 0;
    int removed = 0;
    int moved = 0;
};

class Toolbar final : public UI::Component
{
public:
    Toolbar();

    void resized() override;
    void paint(UI::Graphics& g) override;

    std::function<void(int)> onHostMaxFps = [](int) {};
    std::function<void()> onSnapshot = [] {};

private:
    void applyToAll();

    UI::Checkbox continuous {"Continuous"};
    UI::Label hostFpsCaption {"Host max fps"};
    UI::ComboBox hostFps;
    UI::Label tileFpsCaption {"Tile max fps"};
    UI::ComboBox tileFps;
    UI::Checkbox msaa {"MSAA x4 all"};
    UI::Checkbox timing {"GPU timing"};
    UI::Button snapshot {"Snapshot 2x"};
};

class StatsFooter final : public UI::Component
{
public:
    void setLines(const std::string& first, const std::string& second);
    void paint(UI::Graphics& g) override;

private:
    std::string top;
    std::string bottom;
};
} // namespace GPUComponentsDemo
