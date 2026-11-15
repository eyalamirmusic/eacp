#include "Panels.h"

#include <algorithm>
#include <cmath>

namespace GPUComponentsDemo
{
namespace
{
constexpr auto pi = 3.14159265f;
constexpr int gridCounts[] = {16, 32, 64};
constexpr int fpsChoices[] = {0, 15, 30, 60};
constexpr int maxChurnTiles = 12;

void addFpsItems(UI::ComboBox& box)
{
    for (auto choice: fpsChoices)
        box.addItem(choice == 0 ? "no cap" : std::to_string(choice) + " fps");

    box.setSelectedIndex(0);
}

int fpsAt(int index)
{
    if (index < 0 || index >= (int) std::size(fpsChoices))
        return 0;

    return fpsChoices[index];
}

void paintPage(UI::Graphics& g, const UI::Rect& area)
{
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::background));
    g.fillRect(area);
}

GPUWidgets::AffineTransform about(const GPUWidgets::AffineTransform& transform,
                                  UI::Point centre)
{
    return GPUWidgets::AffineTransform::translation(-centre.x, -centre.y)
        .then(transform)
        .then(GPUWidgets::AffineTransform::translation(centre.x, centre.y));
}
} // namespace

void TileShelf::setTileSize(UI::Point size)
{
    tileSize = size;
    resized();
}

int TileShelf::columnsFor(float width) const
{
    return std::max(1, (int) std::floor((width - gap) / (tileSize.x + gap)));
}

float TileShelf::heightFor(float width) const
{
    auto columns = columnsFor(width);
    auto rows = (tiles.size() + columns - 1) / columns;

    return gap + (float) rows * (tileSize.y + gap);
}

TileFrame& TileShelf::add(TileKind kind)
{
    auto& slot = tiles.create();
    slot.create(kind);

    wire(*slot);
    addAndMakeVisible(*slot);
    resized();
    onTilesChanged();

    return *slot;
}

void TileShelf::wire(TileFrame& frame)
{
    auto closeLater = [this](TileFrame& closing)
    {
        auto* target = &closing;
        auto closeIfHeld = [this, target]
        {
            if (holds(target))
                remove(*target);
        };

        Threads::callAsync(closeIfHeld);
    };

    auto moveLater = [this](TileFrame& moving)
    {
        auto* target = &moving;
        auto moveIfHeld = [this, target]
        {
            if (holds(target))
                onMoveRequested(*target);
        };

        Threads::callAsync(moveIfHeld);
    };

    frame.onClose = closeLater;
    frame.onMove = moveLater;
}

int TileShelf::indexOf(const TileFrame* frame) const
{
    for (auto i = 0; i < tiles.size(); ++i)
        if (tiles[i].get() == frame)
            return i;

    return -1;
}

bool TileShelf::holds(const TileFrame* frame) const
{
    return indexOf(frame) >= 0;
}

void TileShelf::moveTile(TileFrame& frame, TileShelf& destination)
{
    auto index = indexOf(&frame);

    if (index < 0 || &destination == this)
        return;

    removeChildComponent(frame);

    auto& slot = destination.tiles.create();
    slot.reset(tiles[index].release());
    tiles.erase(tiles.begin() + index);

    destination.wire(frame);
    destination.addAndMakeVisible(frame);

    resized();
    destination.resized();
    onTilesChanged();
    destination.onTilesChanged();
}

void TileShelf::remove(TileFrame& frame)
{
    auto index = indexOf(&frame);

    if (index < 0)
        return;

    removeChildComponent(frame);
    tiles.erase(tiles.begin() + index);

    resized();
    onTilesChanged();
}

void TileShelf::clear()
{
    for (auto& tile: tiles)
        removeChildComponent(*tile);

    tiles.clear();
    onTilesChanged();
}

int TileShelf::getNumTiles() const
{
    return tiles.size();
}

TileFrame& TileShelf::getTileFrame(int index) const
{
    return *tiles[index];
}

void TileShelf::resized()
{
    auto columns = columnsFor(getWidth());
    auto width = (getWidth() - gap * (float) (columns + 1)) / (float) columns;

    for (auto i = 0; i < tiles.size(); ++i)
    {
        auto column = i % columns;
        auto row = i / columns;

        tiles[i]->setBounds({gap + (float) column * (width + gap),
                             gap + (float) row * (tileSize.y + gap),
                             width,
                             tileSize.y});
    }
}

void TileShelf::paint(UI::Graphics& g)
{
    g.setColour({0.13f, 0.14f, 0.17f, 1.f});
    g.fillRect(getLocalBounds());
}

GridPanel::GridPanel()
{
    for (auto value: gridCounts)
        count.addItem(std::to_string(value) + " tiles");

    auto onCount = [this](int index)
    {
        if (index >= 0)
            setTileCount(gridCounts[index]);
    };

    auto onWidth = [this](float) { resized(); };

    count.onChange = onCount;
    viewportWidth.setValue(1.f);
    viewportWidth.setDefaultValue(1.f);
    viewportWidth.onValueChange = onWidth;
    hint.setThemeColour(UI::ThemeKeys::dimText);

    viewport.setContent(shelf);
    viewport.setScrollbarsShown(false, true);

    addChildren({countCaption, count, widthCaption, viewportWidth, hint, viewport});

    count.setSelectedIndex(0);
    setTileCount(gridCounts[0]);
}

void GridPanel::setTileCount(int countToShow)
{
    shelf.clear();

    for (auto i = 0; i < countToShow; ++i)
        shelf.add(static_cast<TileKind>(i % tileKindCount));

    layOutShelf();
}

void GridPanel::layOutShelf()
{
    auto width = std::max(0.f, viewport.getWidth() - 10.f);
    viewport.setContentSize(width, shelf.heightFor(width));
}

void GridPanel::resized()
{
    auto area = getLocalBounds().inset(10.f);
    auto row = area.removeFromTop(26.f);

    countCaption.setBounds(row.removeFromLeft(40.f));
    count.setBounds(row.removeFromLeft(110.f));
    row.removeFromLeft(20.f);
    widthCaption.setBounds(row.removeFromLeft(100.f));
    viewportWidth.setBounds(row.removeFromLeft(160.f));
    row.removeFromLeft(20.f);
    hint.setBounds(row);

    area.removeFromTop(8.f);

    auto fraction = 0.35f + 0.65f * viewportWidth.getValue();
    viewport.setBounds(area.withWidth(area.w * fraction));

    layOutShelf();
}

void GridPanel::paint(UI::Graphics& g)
{
    paintPage(g, getLocalBounds());
}

FadingGroup::Contents::Contents()
{
    caption.setThemeColour(UI::ThemeKeys::text);
    knobLike.setValue(0.3f);
    addChildren({caption, tile, knobLike});
}

void FadingGroup::Contents::resized()
{
    auto area = getLocalBounds().inset(8.f);

    caption.setBounds(area.removeFromTop(20.f));
    knobLike.setBounds(area.removeFromBottom(22.f));
    area.removeFromBottom(4.f);
    tile.setBounds(area);
}

void FadingGroup::Contents::paint(UI::Graphics& g)
{
    g.setColour({0.22f, 0.2f, 0.3f, 1.f});
    g.fillRoundedRect(getLocalBounds(), 8.f);
}

FadingGroup::FadingGroup()
{
    addAndMakeVisible(contents);
    layer.setContent(&contents);
}

void FadingGroup::setFade(float opacity)
{
    layer.setOpacity(opacity);
    repaint();
}

void FadingGroup::resized()
{
    contents.setBounds(getLocalBounds());
    layer.setBounds(getLocalBounds());
}

void FadingGroup::paint(UI::Graphics& g)
{
    g.drawLayer(layer);
}

OverlayCard::OverlayCard()
{
    auto onClicked = [this]
    {
        ++clickCount;
        clicks.setText(std::to_string(clickCount) + " clicks");
    };

    button.onClick = onClicked;
    clicks.setThemeColour(UI::ThemeKeys::dimText);
    setInterceptsMouseClicks(true);

    addChildren({text, button, clicks});
}

void OverlayCard::resized()
{
    auto area = getLocalBounds().inset(10.f);

    text.setBounds(area.removeFromTop(20.f));
    area.removeFromTop(8.f);

    auto row = area.removeFromTop(28.f);
    button.setBounds(row.removeFromLeft(90.f));
    row.removeFromLeft(10.f);
    clicks.setBounds(row);
}

void OverlayCard::paint(UI::Graphics& g)
{
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel).withAlpha(0.88f));
    g.fillRoundedRect(getLocalBounds(), 8.f);
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::outline));
    g.drawRoundedRect(getLocalBounds(), 8.f);
}

OverlapPanel::OverlapPanel()
{
    auto onOpacity = [this](float value) { mesh.getTile().setOpacity(value); };
    auto onTransform = [this](float) { placeTransform(); };
    auto onFade = [this](float value) { group.setFade(value); };

    opacity.setValue(0.85f);
    opacity.setDefaultValue(1.f);
    opacity.onValueChange = onOpacity;

    rotation.setValue(0.56f);
    rotation.setDefaultValue(0.5f);
    rotation.onValueChange = onTransform;

    scale.setValue(0.45f);
    scale.setDefaultValue(0.5f);
    scale.onValueChange = onTransform;

    fade.setValue(0.75f);
    fade.setDefaultValue(1.f);
    fade.onValueChange = onFade;

    for (auto i = 1; i <= 30; ++i)
        longList.addItem("Option " + std::to_string(i));

    menuHint.setThemeColour(UI::ThemeKeys::dimText);
    comboCaption.setThemeColour(UI::ThemeKeys::dimText);

    addChildren({opacityCaption, opacity, rotationCaption, rotation});
    addChildren({scaleCaption, scale, fadeCaption, fade, menuHint});
    addChildren({mesh, plasma, feedback, onDemand, group, card});
    addChildren({comboCaption, longList});

    mesh.getTile().setOpacity(opacity.getValue());
    group.setFade(fade.getValue());
}

void OverlapPanel::placeTransform()
{
    auto& tile = mesh.getTile();
    auto angle = (rotation.getValue() - 0.5f) * 2.f * pi;
    auto factor = 0.5f + scale.getValue();
    auto turn = GPUWidgets::AffineTransform::rotation(angle).then(
        GPUWidgets::AffineTransform::scaling(factor, factor));

    tile.setTransform(
        about(turn, {tile.getWidth() * 0.5f, tile.getHeight() * 0.5f}));
}

void OverlapPanel::resized()
{
    auto area = getLocalBounds().inset(10.f);
    auto controls = area.removeFromLeft(220.f);

    auto place = [&](UI::Component& caption, UI::Component& control)
    {
        caption.setBounds(controls.removeFromTop(20.f));
        control.setBounds(controls.removeFromTop(24.f));
        controls.removeFromTop(10.f);
    };

    place(opacityCaption, opacity);
    place(rotationCaption, rotation);
    place(scaleCaption, scale);
    place(fadeCaption, fade);
    menuHint.setBounds(controls.removeFromTop(20.f));

    auto x = area.x + 10.f;
    auto y = area.y;

    mesh.setBounds({x, y + 20.f, 340.f, 260.f});
    plasma.setBounds({x + 230.f, y + 150.f, 320.f, 250.f});
    feedback.setBounds({x + 600.f, y + 20.f, 300.f, 240.f});
    card.setBounds({x + 470.f, y + 200.f, 230.f, 90.f});
    onDemand.setBounds({x, y + 330.f, 220.f, 220.f});
    group.setBounds({x + 580.f, y + 320.f, 320.f, 260.f});

    comboCaption.setBounds({x + 360.f, y, 220.f, 18.f});
    longList.setBounds({x + 360.f, y + 20.f, 180.f, 26.f});

    placeTransform();
}

void OverlapPanel::paint(UI::Graphics& g)
{
    paintPage(g, getLocalBounds());
}

ChurnPanel::ChurnPanel()
{
    auto onAdd = [this] { addTile(); };
    auto onRemove = [this] { removeTile(); };
    auto onReparent = [this] { reparentTile(); };
    auto onChurn = [this](bool on) { setChurning(on); };
    auto onMoveRequested = [this](TileFrame& frame) { moveToOtherShelf(frame); };
    auto onTilesChanged = [this] { updateCounts(); };

    addButton.onClick = onAdd;
    removeButton.onClick = onRemove;
    reparentButton.onClick = onReparent;
    churn.onChange = onChurn;

    for (auto* shelf: {&left, &right})
    {
        shelf->setTileSize({200.f, 160.f});
        shelf->onMoveRequested = onMoveRequested;
        shelf->onTilesChanged = onTilesChanged;
    }

    counts.setThemeColour(UI::ThemeKeys::dimText);

    addChildren({addButton, removeButton, reparentButton, churn, counts});
    addChildren({leftCaption, rightCaption, left, right});

    for (auto i = 0; i < 3; ++i)
    {
        left.add(static_cast<TileKind>(i));
        right.add(static_cast<TileKind>(i + 3));
    }

    added = totalTiles();
    updateCounts();
}

ChurnPanel::~ChurnPanel()
{
    churnTimer.reset();
}

int ChurnPanel::totalTiles() const
{
    return left.getNumTiles() + right.getNumTiles();
}

TileShelf& ChurnPanel::randomShelf()
{
    return random() % 2 == 0 ? left : right;
}

TileShelf& ChurnPanel::shelfHolding(TileFrame& frame)
{
    return left.holds(&frame) ? left : right;
}

void ChurnPanel::addTile()
{
    if (totalTiles() >= maxChurnTiles)
        return;

    auto& shelf = left.getNumTiles() <= right.getNumTiles() ? left : right;
    shelf.add(static_cast<TileKind>(random() % tileKindCount));

    ++added;
    updateCounts();
}

void ChurnPanel::removeTile()
{
    auto& shelf = left.getNumTiles() >= right.getNumTiles() ? left : right;

    if (shelf.getNumTiles() == 0)
        return;

    auto pick = (int) (random() % (unsigned) shelf.getNumTiles());

    for (auto i = 0; i < shelf.getNumTiles(); ++i)
    {
        auto index = (pick + i) % shelf.getNumTiles();

        if (shelf.getTileFrame(index).getTile().isContinuous())
        {
            pick = index;
            break;
        }
    }

    shelf.remove(shelf.getTileFrame(pick));

    ++removed;
    updateCounts();
}

void ChurnPanel::reparentTile()
{
    auto& from = randomShelf();
    auto& source = from.getNumTiles() > 0 ? from : (&from == &left ? right : left);

    if (source.getNumTiles() == 0)
        return;

    auto pick = (int) (random() % (unsigned) source.getNumTiles());
    moveToOtherShelf(source.getTileFrame(pick));
}

void ChurnPanel::moveToOtherShelf(TileFrame& frame)
{
    auto& from = shelfHolding(frame);
    auto& to = &from == &left ? right : left;

    from.moveTile(frame, to);

    ++moved;
    updateCounts();
}

void ChurnPanel::churnOnce()
{
    auto roll = random() % 10;

    if (roll < 4)
        addTile();
    else if (roll < 7)
        removeTile();
    else
        reparentTile();
}

void ChurnPanel::setChurning(bool shouldChurn)
{
    if (!shouldChurn)
    {
        churnTimer.reset();
        return;
    }

    auto tick = [this] { churnOnce(); };
    churnTimer.create(tick, 6);
}

void ChurnPanel::updateCounts()
{
    counts.setText(std::to_string(totalTiles()) + " alive   " + std::to_string(added)
                   + " added   " + std::to_string(removed) + " removed   "
                   + std::to_string(moved) + " reparented");
}

void ChurnPanel::resized()
{
    auto area = getLocalBounds().inset(10.f);
    auto row = area.removeFromTop(28.f);

    addButton.setBounds(row.removeFromLeft(70.f));
    row.removeFromLeft(6.f);
    removeButton.setBounds(row.removeFromLeft(80.f));
    row.removeFromLeft(6.f);
    reparentButton.setBounds(row.removeFromLeft(90.f));
    row.removeFromLeft(16.f);
    churn.setBounds(row.removeFromLeft(190.f));
    row.removeFromLeft(16.f);
    counts.setBounds(row);

    area.removeFromTop(8.f);

    auto half = area.removeFromLeft(area.w * 0.5f - 5.f);
    area.removeFromLeft(10.f);

    leftCaption.setBounds(half.removeFromTop(20.f));
    left.setBounds(half);
    rightCaption.setBounds(area.removeFromTop(20.f));
    right.setBounds(area);
}

void ChurnPanel::paint(UI::Graphics& g)
{
    paintPage(g, getLocalBounds());
}

Toolbar::Toolbar()
{
    auto onContinuous = [this](bool on)
    {
        tileSettings().continuous = on;
        applyToAll();
    };

    auto onHostFps = [this](int index) { onHostMaxFps(fpsAt(index)); };

    auto onTileFps = [this](int index)
    {
        tileSettings().maxFps = fpsAt(index);
        applyToAll();
    };

    auto onMsaa = [](bool on)
    {
        tileSettings().msaa = on;

        for (auto* frame: liveTileFrames())
        {
            frame->getTile().applyMsaa(on);
            frame->getTile().repaint();
        }
    };

    auto onTiming = [this](bool on)
    {
        tileSettings().timing = on;
        applyToAll();
    };

    auto onSnapshotClicked = [this] { onSnapshot(); };

    continuous.setChecked(tileSettings().continuous);
    continuous.onChange = onContinuous;

    addFpsItems(hostFps);
    hostFps.onChange = onHostFps;

    addFpsItems(tileFps);
    tileFps.onChange = onTileFps;

    msaa.onChange = onMsaa;
    timing.onChange = onTiming;
    snapshot.onClick = onSnapshotClicked;

    for (auto* caption: {&hostFpsCaption, &tileFpsCaption})
        caption->setThemeColour(UI::ThemeKeys::dimText);

    addChildren({continuous, hostFpsCaption, hostFps, tileFpsCaption, tileFps});
    addChildren({msaa, timing, snapshot});
}

void Toolbar::applyToAll()
{
    for (auto* frame: liveTileFrames())
    {
        frame->getTile().apply(tileSettings());
        frame->getTile().repaint();
        frame->syncControls();
    }
}

void Toolbar::resized()
{
    auto row = getLocalBounds().inset(10.f, 6.f);

    continuous.setBounds(row.removeFromLeft(100.f));
    row.removeFromLeft(16.f);
    hostFpsCaption.setBounds(row.removeFromLeft(86.f));
    hostFps.setBounds(row.removeFromLeft(90.f));
    row.removeFromLeft(16.f);
    tileFpsCaption.setBounds(row.removeFromLeft(80.f));
    tileFps.setBounds(row.removeFromLeft(90.f));
    row.removeFromLeft(16.f);
    msaa.setBounds(row.removeFromLeft(110.f));
    row.removeFromLeft(8.f);
    timing.setBounds(row.removeFromLeft(100.f));
    row.removeFromLeft(8.f);
    snapshot.setBounds(row.removeFromLeft(100.f));
}

void Toolbar::paint(UI::Graphics& g)
{
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
    g.fillRect(getLocalBounds());
}

void StatsFooter::setLines(const std::string& first, const std::string& second)
{
    if (first == top && second == bottom)
        return;

    top = first;
    bottom = second;
    repaint();
}

void StatsFooter::paint(UI::Graphics& g)
{
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
    g.fillRect(getLocalBounds());

    auto area = getLocalBounds().inset(10.f, 3.f);
    auto line = area.h * 0.5f;

    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::text));
    g.drawText(top, area.removeFromTop(line));
    g.setColour(UI::defaultTheme().get(UI::ThemeKeys::dimText));
    g.drawText(bottom, area);
}
} // namespace GPUComponentsDemo
