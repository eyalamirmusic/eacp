#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace Graphics;

struct TodoItemView final : View
{
    TodoItemView(const std::string& text = "")
        : itemText(text)
    {
        getProperties().handlesMouseEvents = true;

        checkboxLayer->setStrokeColor({0.6f, 0.6f, 0.6f});
        checkboxLayer->setStrokeWidth(2.f);

        checkmarkLayer->setFillColor({0.3f, 0.8f, 0.4f});
        checkmarkLayer->setOpacity(0.f);

        textLayer->setText(text);
        textLayer->setColor({0.9f, 0.9f, 0.9f});

        addChildren({checkboxLayer, checkmarkLayer, textLayer});
    }

    void setCompleted(bool value)
    {
        completed = value;
        checkmarkLayer->setOpacity(completed ? 1.f : 0.f);
        textLayer->setColor(completed ? Color {0.5f, 0.5f, 0.5f}
                                      : Color {0.9f, 0.9f, 0.9f});
    }

    void mouseDown(const MouseEvent&) override { setCompleted(!completed); }

    void mouseEntered(const MouseEvent&) override
    {
        checkboxLayer->setStrokeColor({0.8f, 0.8f, 0.8f});
    }

    void mouseExited(const MouseEvent&) override
    {
        checkboxLayer->setStrokeColor({0.6f, 0.6f, 0.6f});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        float checkboxSize = 20.f;
        float padding = 10.f;

        auto checkboxPath = Path();
        checkboxPath.addRoundedRect(
            {padding, (bounds.h - checkboxSize) / 2.f, checkboxSize, checkboxSize},
            4.f);
        checkboxLayer->setPath(checkboxPath);

        auto checkmarkPath = Path();
        float cx = padding + checkboxSize / 2.f;
        float cy = bounds.h / 2.f;
        checkmarkPath.addEllipse({cx - 6.f, cy - 6.f, 12.f, 12.f});
        checkmarkLayer->setPath(checkmarkPath);

        scaleToFit({checkboxLayer, checkmarkLayer, textLayer});
        textLayer->setPosition(
            {padding * 2 + checkboxSize + 10.f, bounds.h / 2.f - 8.f});
    }

    bool completed = false;
    std::string itemText;

    ShapeLayerView checkboxLayer;
    ShapeLayerView checkmarkLayer;
    TextLayerView textLayer;
};

struct TodoHeaderView final : View
{
    TodoHeaderView()
    {
        titleLayer->setText("Todo List");
        titleLayer->setFont({"Helvetica-Bold", 24.f});
        titleLayer->setColor({0.9f, 0.9f, 0.9f});

        addChildren({titleLayer});
    }

    void resized() override
    {
        scaleToFit({titleLayer});
        titleLayer->setPosition({20.f, 15.f});
    }

    TextLayerView titleLayer;
};

struct TodoListView final : View
{
    TodoListView()
    {
        backgroundLayer->setFillColor({0.15f, 0.15f, 0.15f});

        addChildren({backgroundLayer, header, item1, item2, item3, item4, item5});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto bgPath = Path();
        bgPath.addRoundedRect(bounds.getRelative({0.05f, 0.05f, 0.9f, 0.9f}), 12.f);
        backgroundLayer->setPath(bgPath);
        scaleToFit({backgroundLayer});

        float contentX = bounds.w * 0.05f + 20.f;
        float contentWidth = bounds.w * 0.9f - 40.f;

        header.setBounds({contentX, bounds.h * 0.85f, contentWidth, 60.f});

        float itemHeight = 40.f;
        float startY = bounds.h * 0.8f - 20.f;

        item1.setBounds(
            {contentX, startY - 0.f * itemHeight, contentWidth, itemHeight});
        item2.setBounds(
            {contentX, startY - 1.f * itemHeight, contentWidth, itemHeight});
        item3.setBounds(
            {contentX, startY - 2.f * itemHeight, contentWidth, itemHeight});
        item4.setBounds(
            {contentX, startY - 3.f * itemHeight, contentWidth, itemHeight});
        item5.setBounds(
            {contentX, startY - 4.f * itemHeight, contentWidth, itemHeight});
    }

    ShapeLayerView backgroundLayer;
    TodoHeaderView header;
    TodoItemView item1 {"Learn eacp framework"};
    TodoItemView item2 {"Build a todo app"};
    TodoItemView item3 {"Add more features"};
    TodoItemView item4 {"Test the application"};
    TodoItemView item5 {"Ship it!"};
};

struct BackgroundView final : View
{
    BackgroundView()
    {
        layer->setFillColor({0.08f, 0.08f, 0.1f});
        addChildren({layer});
    }

    void resized() override
    {
        auto path = Path();
        path.addRect(getLocalBounds());
        layer->setPath(path);
        layer.scaleToFit();
    }

    ShapeLayerView layer;
};

struct ContentView final : View
{
    ContentView() { addChildren({background, todoList}); }

    void resized() override { scaleToFit({background, todoList}); }

    BackgroundView background;
    TodoListView todoList;
};

struct TodoApp
{
    TodoApp() { window.setContentView(contentView); }

    ContentView contentView;
    Window window;
};

int main()
{
    eacp::Apps::run<TodoApp>();

    return 0;
}
