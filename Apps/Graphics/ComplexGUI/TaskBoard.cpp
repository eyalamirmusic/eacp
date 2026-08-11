#include <eacp/UI/UI.h>

#include <string>

using namespace eacp;

namespace
{
constexpr auto cardHeight = 62.f;
constexpr auto cardGap = 8.f;
constexpr auto headerHeight = 44.f;
constexpr auto dragStartDistance = 4.f;

Random randomGen {};

std::size_t nextRandom(std::size_t min, std::size_t max)
{
    return randomGen.get(min, max);
}

template <typename T>
auto& getRandomElement(T& container)
{
    return container[nextRandom(0, container.size() - 1)];
}

struct TaskData
{
    int id = 0;
    std::string title;
    std::string description;
    UI::Color colour;
};

void paintCard(UI::Graphics& g,
               const UI::Rect& bounds,
               const TaskData& data,
               bool selected,
               bool hovered,
               bool compact)
{
    const auto& theme = UI::defaultTheme();

    g.setColour(UI::Color::gray(0.23f).withAlpha(hovered ? 1.f : 0.9f));
    g.fillRoundedRect(bounds, 8.f);

    g.setColour(selected ? UI::Color {0.4f, 0.6f, 1.f, 1.f} : theme.outline);
    g.drawRoundedRect(bounds, 8.f, selected ? 2.f : 1.f);

    g.setColour(data.colour);
    g.fillRoundedRect({bounds.x, bounds.y, 4.f, bounds.h}, 2.f);

    auto text = bounds.inset(12.f, 10.f);
    text.x = bounds.x + 12.f;
    text.w = bounds.w - 20.f;

    g.setFontStyle(UI::FontStyle::Bold);
    g.setColour(UI::Color::gray(0.95f));
    g.drawText(data.title,
               compact ? text : text.removeFromTop(text.h * 0.5f),
               UI::Justification::Left);

    if (compact)
        return;

    g.setFontStyle(UI::FontStyle::Regular);
    g.setFontSize(11.f);
    g.setColour(UI::Color::gray(0.7f));
    g.drawText(data.description, text);
}

struct TaskCard final : UI::Component
{
    explicit TaskCard(const TaskData& taskData)
        : data(taskData)
    {
        setInterceptsMouseClicks(true);
        setWantsKeyboardFocus(true);
        setMouseCursor(eacp::Graphics::MouseCursor::PointingHand);
    }

    void setSelected(bool shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }

    void mouseEnter(const UI::MouseEvent&) override { repaint(); }
    void mouseExit(const UI::MouseEvent&) override { repaint(); }

    void mouseDown(const UI::MouseEvent&) override { onSelect(this); }

    void mouseDrag(const UI::MouseEvent& event) override
    {
        auto* container = findDragContainer();

        if (container == nullptr)
            return;

        if (!container->isDragging())
        {
            auto travelled = event.position - event.downPosition;

            if (std::abs(travelled.x) + std::abs(travelled.y) < dragStartDistance)
                return;

            auto info = UI::DragInfo {};
            info.type = "card";
            info.itemId = data.id;

            container->startDragging(
                info,
                *this,
                {180.f, 44.f},
                [this](UI::Graphics& g, const UI::Rect& bounds)
                { paintCard(g, bounds, data, false, true, true); });
        }

        container->dragTo(localPointToRoot(event.position));
    }

    void mouseUp(const UI::MouseEvent& event) override
    {
        if (auto* container = findDragContainer())
            container->drop(localPointToRoot(event.position));
    }

    void paint(UI::Graphics& g) override
    {
        paintCard(g, getLocalBounds(), data, selected, isMouseOver(), false);
    }

    TaskData data;
    bool selected = false;

    std::function<void(TaskCard*)> onSelect = [](TaskCard*) {};
};

struct Column final : UI::Component
{
    Column(std::string columnName, const UI::Color& headerColourToUse)
        : name(std::move(columnName))
        , headerColour(headerColourToUse)
    {
        add.onClick = [this] { onAddCard(); };

        dropTarget.isInterestedIn = [this](const UI::DragInfo& info)
        { return info.type == "card" && findCard(info.itemId) == nullptr; };

        dropTarget.itemDragEnter = [this](const UI::DragInfo&)
        {
            highlighted = true;
            repaint();
        };

        dropTarget.itemDragExit = [this](const UI::DragInfo&)
        {
            highlighted = false;
            repaint();
        };

        dropTarget.itemDropped = [this](const UI::DragInfo& info)
        { onCardDropped(info.itemId, *this); };

        addAndMakeVisible(add);
    }

    TaskCard& addCard(const TaskData& data)
    {
        auto& card = cards.createNew(data);
        card.onSelect = [this](auto* c) { onCardSelect(c); };

        addAndMakeVisible(card);
        resized();
        repaint();

        return card;
    }

    void removeCard(TaskCard& card)
    {
        removeChildComponent(card);

        for (auto i = 0; i < cards.size(); ++i)
        {
            if (cards[i].get() != &card)
                continue;

            cards.removeAt(i);
            break;
        }

        resized();
        repaint();
    }

    TaskCard* findCard(int id) const
    {
        for (auto& card: cards)
            if (card->data.id == id)
                return card.get();

        return nullptr;
    }

    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour(UI::Color::gray(highlighted ? 0.22f : 0.16f));
        g.fillRoundedRect(bounds, 10.f);

        g.setColour(headerColour.withAlpha(0.3f));
        g.fillRoundedRect(bounds.fromTop(headerHeight), 10.f);

        auto header = bounds.fromTop(headerHeight).inset(12.f, 6.f);

        g.setFontStyle(UI::FontStyle::Bold);
        g.setFontSize(14.f);
        g.setColour(UI::Color::gray(0.9f));
        g.drawText(name, header.removeFromTop(18.f));

        g.setFontStyle(UI::FontStyle::Regular);
        g.setFontSize(11.f);
        g.setColour(UI::Color::gray(0.6f));
        g.drawText(std::to_string(cards.size()) + " tasks", header);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        add.setBounds(bounds.fromTop(26.f, 8.f).fromRight(26.f, 8.f));

        auto area = bounds.inset(8.f, 0.f);
        area.removeFromTop(headerHeight + cardGap);

        for (auto& card: cards)
        {
            card->setBounds(area.removeFromTop(cardHeight));
            area.removeFromTop(cardGap);
        }
    }

    std::string name;
    UI::Color headerColour;
    bool highlighted = false;

    OwnedVector<TaskCard> cards;
    UI::Button add {"+"};
    UI::DragAndDropTarget dropTarget {*this};

    std::function<void(TaskCard*)> onCardSelect = [](TaskCard*) {};
    std::function<void(int, Column&)> onCardDropped = [](int, Column&) {};
    std::function<void()> onAddCard = [] {};
};

struct Board final : UI::Component
{
    Board()
    {
        setInterceptsMouseClicks(true);
        setWantsKeyboardFocus(true);

        for (auto* column: columns())
        {
            column->onCardSelect = [this](auto* card) { selectCard(card); };
            column->onCardDropped = [this](int id, Column& into)
            { moveCard(id, into); };
            column->onAddCard = [this, column] { addCardTo(*column); };

            addAndMakeVisible(*column);
        }

        clearAll.onClick = [this] { removeEveryCard(); };
        addSample.onClick = [this] { addSampleCards(); };

        addChildren({clearAll, addSample});

        addSampleCards();
    }

    Array<Column*, 3> columns() { return {&todo, &progress, &done}; }

    void addSampleCards()
    {
        static const auto titles = Vector<std::string> {"Design the architecture",
                                                        "Implement user auth",
                                                        "Write unit tests",
                                                        "Review pull request",
                                                        "Fix the memory leak",
                                                        "Update documentation",
                                                        "Optimize the database",
                                                        "Deploy to staging"};

        static const auto descriptions = Vector<std::string> {"High priority",
                                                              "Needs review",
                                                              "In progress",
                                                              "Blocked",
                                                              "Ready for QA"};

        for (auto i = 0; i < 3; ++i)
        {
            auto data =
                makeCard(getRandomElement(titles), getRandomElement(descriptions));

            columns()[(int) nextRandom(0, 2)]->addCard(data);
        }

        repaint();
    }

    TaskData makeCard(const std::string& title, const std::string& description)
    {
        static const auto colours = Vector<UI::Color> {{0.4f, 0.6f, 1.0f, 1.f},
                                                       {1.0f, 0.5f, 0.3f, 1.f},
                                                       {0.5f, 0.8f, 0.4f, 1.f},
                                                       {0.9f, 0.4f, 0.6f, 1.f},
                                                       {0.6f, 0.4f, 0.9f, 1.f}};

        return {nextId++, title, description, getRandomElement(colours)};
    }

    void addCardTo(Column& column)
    {
        auto data =
            makeCard("New task " + std::to_string(nextId), "Click to select");

        selectCard(&column.addCard(data));
    }

    void selectCard(TaskCard* card)
    {
        if (selected != nullptr)
            selected->setSelected(false);

        selected = card;

        if (selected != nullptr)
            selected->setSelected(true);
    }

    Column* columnHolding(int id)
    {
        for (auto* column: columns())
            if (column->findCard(id) != nullptr)
                return column;

        return nullptr;
    }

    // The card is rebuilt rather than reparented, which is why a drag carries an
    // id rather than a pointer.
    void moveCard(int id, Column& into)
    {
        auto* from = columnHolding(id);

        if (from == nullptr || from == &into)
            return;

        auto* card = from->findCard(id);
        auto data = card->data;

        if (card == selected)
            selected = nullptr;

        from->removeCard(*card);

        selectCard(&into.addCard(data));
        into.highlighted = false;

        repaint();
    }

    void removeEveryCard()
    {
        selected = nullptr;

        for (auto* column: columns())
            while (column->cards.size() > 0)
                column->removeCard(*column->cards[0]);

        repaint();
    }

    void moveSelected(int direction)
    {
        if (selected == nullptr)
            return;

        auto* from = columnHolding(selected->data.id);
        auto all = columns();

        for (auto i = 0; i < all.size(); ++i)
        {
            if (all[i] != from)
                continue;

            auto target = i + direction;

            if (target >= 0 && target < all.size())
                moveCard(selected->data.id, *all[target]);

            return;
        }
    }

    bool keyDown(const UI::KeyEvent& event) override
    {
        using namespace eacp::Graphics;

        if (event.keyCode == KeyCode::N)
        {
            addCardTo(todo);
            return true;
        }

        if (event.keyCode == KeyCode::Delete && selected != nullptr)
        {
            if (auto* column = columnHolding(selected->data.id))
            {
                auto* card = selected;
                selected = nullptr;
                column->removeCard(*card);
            }

            return true;
        }

        if (event.keyCode == KeyCode::RightArrow)
        {
            moveSelected(1);
            return true;
        }

        if (event.keyCode == KeyCode::LeftArrow)
        {
            moveSelected(-1);
            return true;
        }

        return false;
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::Color::gray(0.1f));

        auto bounds = getLocalBounds();

        g.setColour(UI::Color::gray(0.13f));
        g.fillRect(bounds.fromTop(60.f));

        g.setFontStyle(UI::FontStyle::Bold);
        g.setFontSize(20.f);
        g.setColour(UI::Color::gray(0.95f));
        g.drawText("Task Board", bounds.fromTop(60.f).inset(20.f, 0.f));

        g.setFontStyle(UI::FontStyle::Regular);
        g.setFontSize(11.f);
        g.setColour(UI::Color::gray(0.6f));
        g.drawText("Drag cards between columns · N adds one · Delete removes the "
                   "selected one · Arrows move it",
                   bounds.fromBottom(26.f).inset(20.f, 0.f));
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto header = bounds.removeFromTop(60.f).inset(20.f, 14.f);
        addSample.setBounds(header.removeFromRight(90.f));
        header.removeFromRight(8.f);
        clearAll.setBounds(header.removeFromRight(90.f));

        bounds.removeFromBottom(26.f);

        auto area = bounds.inset(16.f, 12.f);
        auto columnWidth = (area.w - cardGap * 2.f) / 3.f;

        for (auto* column: columns())
        {
            column->setBounds(area.removeFromLeft(columnWidth));
            area.removeFromLeft(cardGap);
        }
    }

    Column todo {"To Do", {0.4f, 0.6f, 1.0f, 1.f}};
    Column progress {"In Progress", {1.0f, 0.7f, 0.3f, 1.f}};
    Column done {"Done", {0.5f, 0.8f, 0.4f, 1.f}};

    UI::Button clearAll {"Clear all"};
    UI::Button addSample {"+ Sample"};

    TaskCard* selected = nullptr;
    int nextId = 1;

    // Declared last so it is destroyed first: a drag holds pointers into the
    // tree above it.
    UI::DragAndDropContainer dragging {*this};
};

struct Host final : UI::ComponentHost
{
    Host()
    {
        setBackgroundColour(UI::Color::gray(0.1f));
        setRootComponent(board);
    }

    Board board;
};

eacp::Graphics::WindowOptions windowOptions()
{
    auto options = eacp::Graphics::WindowOptions {};

    options.width = 1000;
    options.height = 640;
    options.title = "Task Board";
    options.minWidth = 640;
    options.minHeight = 420;

    return options;
}

struct TaskBoardApp
{
    TaskBoardApp()
    {
        window.setContentView(host);
        host.focus();
        host.board.grabKeyboardFocus();
    }

    Host host;
    eacp::Graphics::Window window {windowOptions()};
};
} // namespace

int main(int argc, char* argv[])
{
    return Apps::run<TaskBoardApp>(argc, argv);
}
