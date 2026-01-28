#include <eacp/Graphics/Graphics.h>
#include <eacp/Core/Core.h>
#include <algorithm>
#include <functional>
#include <memory>

using namespace eacp;
using namespace Graphics;

Random randomGen{};

size_t nextRandom(size_t min, size_t max)
{
    return randomGen.getNext(min, max);
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
    Color color;
};

struct Button final : View
{
    std::function<void()> onClick;

    Button(const std::string& label, Color bgColor = Color::gray(0.33f))
        : backgroundColor(bgColor)
    {
        setHandlesMouseEvents();
        textLayer->setText(label);
        textLayer->setColor(Color::gray(0.9f));
        addChildren({backgroundLayer, textLayer});
        updateAppearance();
    }

    void mouseEntered(const MouseEvent&) override { updateAppearance(); }
    void mouseExited(const MouseEvent&) override { updateAppearance(); }

    void mouseDown(const MouseEvent&) override
    {
        pressed = true;
        updateAppearance();
    }

    void mouseUp(const MouseEvent& e) override
    {
        if (pressed && getLocalBounds().contains(e.pos))
        {
            if (onClick)
                onClick();
        }
        pressed = false;
        updateAppearance();
    }

    void updateAppearance()
    {
        auto alpha = 0.7f;
        if (isHovering())
            alpha = 0.9f;
        if (pressed)
            alpha = 1.0f;

        backgroundLayer->setFillColor(backgroundColor.withAlpha(alpha));
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto path = Path();
        path.addRoundedRect(bounds, 6.f);
        backgroundLayer->setPath(path);
        scaleToFit({backgroundLayer, textLayer});
        textLayer->setPosition({8.f, bounds.h / 2.f - 7.f});
    }

    bool pressed = false;
    Color backgroundColor;
    ShapeLayerView backgroundLayer;
    TextLayerView textLayer;
};

struct TaskCard final : View
{
    TaskData data;
    std::function<void(TaskCard*)> onSelect;
    std::function<void(TaskCard*)> onDelete;
    std::function<void(TaskCard*, Point)> onDragStart;

    TaskCard(const TaskData& taskData)
        : data(taskData)
    {
        setHandlesMouseEvents().setGrabsFocusOnMouseDown();

        titleLayer->setText(data.title);
        titleLayer->setColor(Color::gray(0.95f));
        titleLayer->setFont(FontOptions().withName("Helvetica-Bold").withSize(13.f));

        descLayer->setText(data.description);
        descLayer->setColor(Color::gray(0.7f));
        descLayer->setFont(FontOptions().withSize(11.f));

        deleteButton.onClick = [this]
        {
            if (onDelete)
                onDelete(this);
        };

        addChildren(
            {backgroundLayer, accentLayer, titleLayer, descLayer, deleteButton});
        updateAppearance();
    }

    void mouseEntered(const MouseEvent&) override { updateAppearance(); }
    void mouseExited(const MouseEvent&) override { updateAppearance(); }

    void mouseDown(const MouseEvent& e) override
    {
        dragStartPos = e.pos;

        if (onSelect)
            onSelect(this);
    }

    void mouseDragged(const MouseEvent& e) override
    {
        auto delta = e.pos - dragStartPos;

        if (delta.length() > 5.f && onDragStart)
            onDragStart(this, e.pos);
    }

    void setSelected(bool sel)
    {
        selected = sel;
        updateAppearance();
    }

    void updateAppearance()
    {
        auto bgAlpha = 0.85f;
        if (isHovering())
            bgAlpha = 0.95f;
        if (selected)
            bgAlpha = 1.0f;

        backgroundLayer->setFillColor(Color::gray(0.23f).withAlpha(bgAlpha));

        if (selected)
            backgroundLayer->setStrokeColor({0.4f, 0.6f, 1.0f});
        else
            backgroundLayer->setStrokeColor(Color::gray(0.33f));
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto bgPath = Path();
        bgPath.addRoundedRect(bounds, 8.f);
        backgroundLayer->setPath(bgPath);
        backgroundLayer->setStrokeWidth(selected ? 2.f : 1.f);

        auto accentPath = Path();
        accentPath.addRoundedRect({0, 0, 4.f, bounds.h}, 2.f);
        accentLayer->setPath(accentPath);
        accentLayer->setFillColor(data.color);

        scaleToFit({backgroundLayer, accentLayer, titleLayer, descLayer});

        titleLayer->setPosition({12.f, bounds.h - 22.f});
        descLayer->setPosition({12.f, bounds.h - 40.f});

        deleteButton.setBounds(bounds.fromRight(22.f, 6.f).fromTop(22.f, 6.f));
    }

    bool selected = false;
    Point dragStartPos;

    ShapeLayerView backgroundLayer;
    ShapeLayerView accentLayer;
    TextLayerView titleLayer;
    TextLayerView descLayer;
    Button deleteButton{"×", {0.5f, 0.2f, 0.2f}};
};

struct Column final : View
{
    std::string name;
    Color headerColor;
    std::vector<std::unique_ptr<TaskCard> > cards;
    std::function<void(TaskCard*)> onCardSelect;
    std::function<void(TaskCard*)> onCardDelete;
    std::function<void(TaskCard*, Point)> onCardDragStart;
    std::function<void()> onAddCard;

    Column(const std::string& columnName, Color color)
        : name(columnName)
          , headerColor(color)
    {
        headerText->setText(columnName);
        headerText->setColor(Color::gray(0.9f));
        headerText->setFont(FontOptions().withName("Helvetica-Bold").withSize(14.f));

        countText->setColor(Color::gray(0.6f));
        countText->setFont(FontOptions().withSize(11.f));

        addButton.onClick = [this]
        {
            if (onAddCard)
                onAddCard();
        };

        addChildren({backgroundLayer, headerBg, headerText, countText, addButton});
        updateCountText();
    }

    TaskCard* addCard(const TaskData& data)
    {
        auto card = std::make_unique<TaskCard>(data);
        auto* cardPtr = card.get();

        card->onSelect = [this](TaskCard* c)
        {
            if (onCardSelect)
                onCardSelect(c);
        };

        card->onDelete = [this](TaskCard* c)
        {
            if (onCardDelete)
                onCardDelete(c);
        };

        card->onDragStart = [this](TaskCard* c, Point p)
        {
            if (onCardDragStart)
                onCardDragStart(c, p);
        };

        addSubview(*card);
        cards.push_back(std::move(card));

        updateCountText();
        layoutCards();
        return cardPtr;
    }

    void removeCard(TaskCard* card)
    {
        auto it = std::find_if(cards.begin(),
                               cards.end(),
                               [card](const auto& c) { return c.get() == card; });

        if (it != cards.end())
        {
            removeSubview(*card);
            cards.erase(it);
            updateCountText();
            layoutCards();
        }
    }

    TaskCard* findCard(int id)
    {
        for (auto& card: cards)
        {
            if (card->data.id == id)
                return card.get();
        }
        return nullptr;
    }

    void updateCountText()
    {
        countText->setText(std::to_string(cards.size()) + " tasks");
    }

    void layoutCards()
    {
        auto bounds = getLocalBounds();
        auto cardY = bounds.h - 60.f;
        auto cardHeight = 60.f;
        auto cardSpacing = 8.f;
        auto cardMargin = 8.f;

        for (auto& card: cards)
        {
            card->setBounds({cardMargin,
                             cardY - cardHeight,
                             bounds.w - cardMargin * 2,
                             cardHeight});
            cardY -= cardHeight + cardSpacing;
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto bgPath = Path();
        bgPath.addRoundedRect(bounds, 10.f);
        backgroundLayer->setPath(bgPath);
        backgroundLayer->setFillColor(Color::gray(0.16f));

        auto headerPath = Path();
        headerPath.addRoundedRect(bounds.fromTop(44.f), 10.f);
        headerBg->setPath(headerPath);
        headerBg->setFillColor(headerColor.withAlpha(0.3f));

        scaleToFit({backgroundLayer, headerBg, headerText, countText});

        headerText->setPosition({12.f, bounds.h - 30.f});
        countText->setPosition({12.f, bounds.h - 12.f});

        addButton.setBounds(bounds.fromRight(26.f, 8.f).fromTop(26.f, 6.f));

        layoutCards();
    }

    ShapeLayerView backgroundLayer;
    ShapeLayerView headerBg;
    TextLayerView headerText;
    TextLayerView countText;
    Button addButton{"+", {0.2f, 0.4f, 0.3f}};
};

struct DragOverlay final : View
{
    DragOverlay()
    {
        textLayer->setColor(Color::gray(0.9f));
        textLayer->setFont(FontOptions().withName("Helvetica-Bold").withSize(12.f));
        addChildren({backgroundLayer, accentLayer, textLayer});
    }

    void setTask(const TaskData& data)
    {
        taskData = data;
        textLayer->setText(data.title);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto bgPath = Path();
        bgPath.addRoundedRect(bounds, 8.f);
        backgroundLayer->setPath(bgPath);
        backgroundLayer->setFillColor(Color::gray(0.27f, 0.95f));

        auto accentPath = Path();
        accentPath.addRoundedRect({0, 0, 4.f, bounds.h}, 2.f);
        accentLayer->setPath(accentPath);
        accentLayer->setFillColor(taskData.color);

        scaleToFit({backgroundLayer, accentLayer, textLayer});
        textLayer->setPosition({12.f, bounds.h / 2.f - 7.f});
    }

    TaskData taskData;
    ShapeLayerView backgroundLayer;
    ShapeLayerView accentLayer;
    TextLayerView textLayer;
};

struct StatusBar final : View
{
    StatusBar()
    {
        statusText->setColor(Color::gray(0.6f));
        statusText->setFont(FontOptions().withSize(11.f));
        addChildren({backgroundLayer, statusText});
    }

    void setStatus(const std::string& text) { statusText->setText(text); }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto path = Path();
        path.addRect(bounds);
        backgroundLayer->setPath(path);
        backgroundLayer->setFillColor(Color::gray(0.11f));

        scaleToFit({backgroundLayer, statusText});
        statusText->setPosition({12.f, bounds.h / 2.f - 6.f});
    }

    ShapeLayerView backgroundLayer;
    TextLayerView statusText{
        "Ready | Press 'N' to add task, Delete to remove, Arrow keys to navigate"};
};

struct Header final : View
{
    std::function<void()> onClearAll;
    std::function<void()> onAddSample;

    Header()
    {
        titleText->setText("Task Board");
        titleText->setColor(Color::gray(0.95f));
        titleText->setFont(FontOptions().withName("Helvetica-Bold").withSize(20.f));

        clearButton.onClick = [this]
        {
            if (onClearAll)
                onClearAll();
        };

        sampleButton.onClick = [this]
        {
            if (onAddSample)
                onAddSample();
        };

        addChildren({backgroundLayer, titleText, clearButton, sampleButton});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto path = Path();
        path.addRect(bounds);
        backgroundLayer->setPath(path);
        backgroundLayer->setFillColor(Color::gray(0.13f));

        scaleToFit({backgroundLayer, titleText});
        titleText->setPosition({20.f, bounds.center().y - 12.f});

        clearButton.setBounds(bounds.fromRight(80.f, 100.f).inset(0.f, 12.f));
        sampleButton.setBounds(bounds.fromRight(80.f, 10.f).inset(0.f, 12.f));
    }

    ShapeLayerView backgroundLayer;
    TextLayerView titleText;
    Button clearButton{"Clear All", {0.5f, 0.2f, 0.2f}};
    Button sampleButton{"+ Sample", {0.2f, 0.4f, 0.6f}};
};

struct TaskBoardView final : View
{
    TaskBoardView()
    {
        setHandlesMouseEvents().setGrabsFocusOnMouseDown();

        setupColumns();
        setupCallbacks();

        addChildren({header, todoColumn, progressColumn, doneColumn, statusBar});

        addSampleTasks();
    }

    void setupColumns()
    {
        todoColumn.onCardSelect = [this](TaskCard* c) { selectCard(c); };
        todoColumn.onCardDelete = [this](TaskCard* c) { deleteCard(c); };
        todoColumn.onCardDragStart = [this](TaskCard* c, Point p)
        {
            startDrag(c, p);
        };
        todoColumn.onAddCard = [this] { addNewTaskToColumn(&todoColumn); };

        progressColumn.onCardSelect = [this](TaskCard* c) { selectCard(c); };
        progressColumn.onCardDelete = [this](TaskCard* c) { deleteCard(c); };
        progressColumn.onCardDragStart = [this](TaskCard* c, Point p)
        {
            startDrag(c, p);
        };
        progressColumn.onAddCard = [this] { addNewTaskToColumn(&progressColumn); };

        doneColumn.onCardSelect = [this](TaskCard* c) { selectCard(c); };
        doneColumn.onCardDelete = [this](TaskCard* c) { deleteCard(c); };
        doneColumn.onCardDragStart = [this](TaskCard* c, Point p)
        {
            startDrag(c, p);
        };
        doneColumn.onAddCard = [this] { addNewTaskToColumn(&doneColumn); };
    }

    void setupCallbacks()
    {
        header.onClearAll = [this] { clearAllTasks(); };
        header.onAddSample = [this] { addSampleTasks(); };
    }

    void addSampleTasks()
    {
        static const std::vector<std::string> sampleTitles = {
            "Design system architecture",
            "Implement user auth",
            "Write unit tests",
            "Review pull request",
            "Fix memory leak",
            "Update documentation",
            "Optimize database",
            "Deploy to staging"};

        static const std::vector<std::string> sampleDescs = {"High priority",
            "Needs review",
            "In progress",
            "Blocked",
            "Ready for QA"};

        for (int i = 0; i < 3; ++i)
        {
            auto& title = getRandomElement(sampleTitles);
            auto& desc = getRandomElement(sampleDescs);

            auto data = createTaskData(title, desc);

            auto col = nextRandom(0, 2);

            if (col == 0)
                todoColumn.addCard(data);
            else if (col == 1)
                progressColumn.addCard(data);
            else
                doneColumn.addCard(data);
        }

        updateStatus();
    }

    TaskData createTaskData(const std::string& title, const std::string& desc)
    {
        static const std::vector<Color> colors = {{0.4f, 0.6f, 1.0f},
                                                  {1.0f, 0.5f, 0.3f},
                                                  {0.5f, 0.8f, 0.4f},
                                                  {0.9f, 0.4f, 0.6f},
                                                  {0.6f, 0.4f, 0.9f}};

        auto& color = getRandomElement(colors);
        return {nextTaskId++, title, desc, color};
    }

    void addNewTaskToColumn(Column* column)
    {
        auto data = createTaskData("New Task " + std::to_string(nextTaskId),
                                   "Click to edit");
        auto* card = column->addCard(data);
        selectCard(card);
        updateStatus();
    }

    void selectCard(TaskCard* card)
    {
        if (selectedCard)
            selectedCard->setSelected(false);

        selectedCard = card;

        if (selectedCard)
            selectedCard->setSelected(true);
    }

    void deleteCard(TaskCard* card)
    {
        if (card == selectedCard)
            selectedCard = nullptr;

        findColumnForCard(card)->removeCard(card);
        updateStatus();
    }

    void clearAllTasks()
    {
        selectedCard = nullptr;

        while (!todoColumn.cards.empty())
            todoColumn.removeCard(todoColumn.cards.front().get());

        while (!progressColumn.cards.empty())
            progressColumn.removeCard(progressColumn.cards.front().get());

        while (!doneColumn.cards.empty())
            doneColumn.removeCard(doneColumn.cards.front().get());

        updateStatus();
    }

    Column* findColumnForCard(TaskCard* card)
    {
        if (todoColumn.findCard(card->data.id))
            return &todoColumn;
        if (progressColumn.findCard(card->data.id))
            return &progressColumn;
        if (doneColumn.findCard(card->data.id))
            return &doneColumn;
        return nullptr;
    }

    void startDrag(TaskCard* card, Point)
    {
        dragging = true;
        draggedCard = card;
        dragOverlay.setTask(card->data);
        addSubview(dragOverlay);
        updateDragPosition();
        selectCard(card);
    }

    void updateDragPosition()
    {
        if (!dragging)
            return;

        auto mousePos = getMousePosition();
        dragOverlay.setBounds({mousePos.x - 100.f, mousePos.y - 30.f, 200.f, 60.f});
    }

    void endDrag()
    {
        if (!dragging)
            return;

        dragging = false;
        removeSubview(dragOverlay);

        if (!draggedCard)
            return;

        auto mousePos = getMousePosition();
        auto* targetColumn = getColumnAtPoint(mousePos);

        if (targetColumn)
        {
            auto* sourceColumn = findColumnForCard(draggedCard);

            if (sourceColumn && sourceColumn != targetColumn)
            {
                auto data = draggedCard->data;
                sourceColumn->removeCard(draggedCard);
                auto* newCard = targetColumn->addCard(data);
                selectCard(newCard);
            }
        }

        draggedCard = nullptr;
        updateStatus();
    }

    Column* getColumnAtPoint(const Point& point)
    {
        if (todoColumn.getBounds().contains(point))
            return &todoColumn;
        if (progressColumn.getBounds().contains(point))
            return &progressColumn;
        if (doneColumn.getBounds().contains(point))
            return &doneColumn;
        return nullptr;
    }

    void mouseMoved(const MouseEvent&) override { updateDragPosition(); }

    void mouseDragged(const MouseEvent&) override { updateDragPosition(); }

    void mouseUp(const MouseEvent&) override { endDrag(); }

    void keyDown(const KeyEvent& event) override
    {
        if (event.keyCode == KeyCode::N)
        {
            addNewTaskToColumn(&todoColumn);
        }
        else if (event.keyCode == KeyCode::Delete && selectedCard)
        {
            deleteCard(selectedCard);
        }
        else if (event.keyCode == KeyCode::RightArrow && selectedCard)
        {
            moveCardRight();
        }
        else if (event.keyCode == KeyCode::LeftArrow && selectedCard)
        {
            moveCardLeft();
        }
        else if (event.keyCode == KeyCode::UpArrow)
        {
            selectPreviousCard();
        }
        else if (event.keyCode == KeyCode::DownArrow)
        {
            selectNextCard();
        }
    }

    void moveCardRight()
    {
        if (!selectedCard)
            return;

        auto* sourceColumn = findColumnForCard(selectedCard);
        Column* targetColumn = nullptr;

        if (sourceColumn == &todoColumn)
            targetColumn = &progressColumn;
        else if (sourceColumn == &progressColumn)
            targetColumn = &doneColumn;

        if (targetColumn)
        {
            auto data = selectedCard->data;
            sourceColumn->removeCard(selectedCard);
            auto* newCard = targetColumn->addCard(data);
            selectCard(newCard);
            updateStatus();
        }
    }

    void moveCardLeft()
    {
        if (!selectedCard)
            return;

        auto* sourceColumn = findColumnForCard(selectedCard);
        Column* targetColumn = nullptr;

        if (sourceColumn == &doneColumn)
            targetColumn = &progressColumn;
        else if (sourceColumn == &progressColumn)
            targetColumn = &todoColumn;

        if (targetColumn)
        {
            auto data = selectedCard->data;
            sourceColumn->removeCard(selectedCard);
            auto* newCard = targetColumn->addCard(data);
            selectCard(newCard);
            updateStatus();
        }
    }

    std::vector<TaskCard*> getAllCards()
    {
        std::vector<TaskCard*> all;
        for (auto& c: todoColumn.cards)
            all.push_back(c.get());
        for (auto& c: progressColumn.cards)
            all.push_back(c.get());
        for (auto& c: doneColumn.cards)
            all.push_back(c.get());
        return all;
    }

    void selectNextCard()
    {
        auto all = getAllCards();
        if (all.empty())
            return;

        if (!selectedCard)
        {
            selectCard(all.front());
            return;
        }

        auto it = std::find(all.begin(), all.end(), selectedCard);
        if (it != all.end() && std::next(it) != all.end())
            selectCard(*std::next(it));
    }

    void selectPreviousCard()
    {
        auto all = getAllCards();
        if (all.empty())
            return;

        if (!selectedCard)
        {
            selectCard(all.back());
            return;
        }

        auto it = std::find(all.begin(), all.end(), selectedCard);

        if (it != all.end() && it != all.begin())
            selectCard(*std::prev(it));
    }

    void updateStatus()
    {
        auto total = todoColumn.cards.size() + progressColumn.cards.size()
                     + doneColumn.cards.size();

        auto status = "Total: " + std::to_string(total) + " tasks | "
                      + "To Do: " + std::to_string(todoColumn.cards.size()) + " | "
                      + "In Progress: " + std::to_string(progressColumn.cards.size())
                      + " | " + "Done: " + std::to_string(doneColumn.cards.size())
                      + " | " + "Press N=new, Del=delete, Arrows=navigate/move";

        statusBar.setStatus(status);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto headerHeight = 54.f;
        auto statusHeight = 28.f;
        auto columnMargin = 12.f;

        header.setBounds({0, bounds.h - headerHeight, bounds.w, headerHeight});
        statusBar.setBounds({0, 0, bounds.w, statusHeight});

        auto contentHeight =
            bounds.h - headerHeight - statusHeight - columnMargin * 2;
        auto contentY = statusHeight + columnMargin;
        auto columnWidth = (bounds.w - columnMargin * 4) / 3.f;

        todoColumn.setBounds({columnMargin, contentY, columnWidth, contentHeight});

        progressColumn.setBounds(
            {columnMargin * 2 + columnWidth, contentY, columnWidth, contentHeight});

        doneColumn.setBounds({columnMargin * 3 + columnWidth * 2,
                              contentY,
                              columnWidth,
                              contentHeight});
    }

    int nextTaskId = 1;
    TaskCard* selectedCard = nullptr;
    bool dragging = false;
    TaskCard* draggedCard = nullptr;

    Header header;
    Column todoColumn{"To Do", {0.4f, 0.6f, 1.0f}};
    Column progressColumn{"In Progress", {1.0f, 0.6f, 0.2f}};
    Column doneColumn{"Done", {0.4f, 0.8f, 0.4f}};
    StatusBar statusBar;
    DragOverlay dragOverlay;
};

struct TaskBoardApp
{
    TaskBoardApp() { window.setContentView(boardView); }

    TaskBoardView boardView;
    Window window;
};

int main()
{
    eacp::Apps::run<TaskBoardApp>();
    return 0;
}