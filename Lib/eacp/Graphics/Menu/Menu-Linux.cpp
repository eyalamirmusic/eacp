#include "LinuxMenuBar-Linux.h"

#include "MenuCommands.h"
#include "../Window/LinuxWindowNative-Linux.h"

#include <eacp/Core/App/App.h>

namespace eacp::Graphics
{
namespace
{
LinuxMenuBarPresenterFactory& linuxMenuBarPresenterFactory()
{
    static auto factory = LinuxMenuBarPresenterFactory {};
    return factory;
}

std::unique_ptr<LinuxMenuBarPresenter> linuxMakePresenter(const MenuBar& bar,
                                                          Callback layoutChanged)
{
    auto visible = visibleMenuBar(bar);
    auto& factory = linuxMenuBarPresenterFactory();

    if (visible.menus.empty() || !factory)
        return {};

    return factory(visible, std::move(layoutChanged));
}
} // namespace

void setLinuxMenuBarPresenterFactory(LinuxMenuBarPresenterFactory factory)
{
    linuxMenuBarPresenterFactory() = std::move(factory);
}

LinuxWindowMenuBar::LinuxWindowMenuBar(LinuxWindowNative& nativeToUse,
                                       const MenuBar& barToUse)
    : native(nativeToUse)
    , surface(nativeToUse.getWindowSurface())
    , input(nativeToUse.getState().events->input)
    , bar(barToUse)
    , presenter(linuxMakePresenter(bar, [this] { layout(); }))
{
    input.addListener(*this);
}

LinuxWindowMenuBar::~LinuxWindowMenuBar()
{
    input.removeListener(*this);
    surface.keyFilter = {};

    if (presenter == nullptr || surface.menuBarView != &presenter->getView())
        return;

    linuxUnbindWindowFromContentView(presenter->getView());

    surface.menuBarView = nullptr;
    surface.menuBarModal = false;
    surface.menuBarDismiss = [] {};
    surface.layoutMenuBar = [] {};
}

float LinuxWindowMenuBar::getHeight() const
{
    return presenter != nullptr ? presenter->getHeight() : 0.f;
}

void LinuxWindowMenuBar::attach()
{
    surface.keyFilter = [this](const KeyEvent& event) { return filterKey(event); };

    if (presenter != nullptr)
    {
        auto& view = presenter->getView();

        surface.menuBarView = &view;
        surface.menuBarDismiss = [this] { presenter->dismissMenu(); };
        surface.layoutMenuBar = [this] { layout(); };

        linuxBindWindowToContentView(view, surface);
    }

    native.setContentTop(getHeight());
}

void LinuxWindowMenuBar::layout()
{
    if (presenter == nullptr)
        return;

    presenter->layout({0.f, 0.f, surface.contentSize.x, surface.contentSize.y});
    surface.menuBarModal = presenter->isMenuOpen();
}

bool LinuxWindowMenuBar::filterKey(const KeyEvent& event)
{
    if (presenter != nullptr && presenter->keyPressed(event))
        return true;

    const auto* item = findShortcutItem(bar, event);

    if (item == nullptr)
        return false;

    // Copied: the action may install another bar, which destroys this one.
    auto action = item->action;
    action();

    return true;
}

void LinuxWindowMenuBar::windowActivationChanged(bool isKey)
{
    if (!isKey && presenter != nullptr && presenter->isMenuOpen())
        presenter->dismissMenu();
}

void setApplicationMenuBar(const MenuBar& bar, Window& window)
{
    auto& native = linuxWindowNative(window);
    auto& installed = native.getState().menuBar;

    // The new one is built before the old goes, and attached after, so the
    // window changes size once, by the difference in height.
    auto next = std::make_unique<LinuxWindowMenuBar>(native, bar);

    installed.reset();
    installed = std::move(next);
    installed->attach();
}

// No Linux desktop has an application menu. Quit goes under File, where KDE
// and GNOME applications keep it; an app with a File menu of its own adds its
// items to this one rather than adding a second.
Menu standardApplicationMenu(std::string)
{
    auto menu = Menu {"File"};

    menu.add(
        MenuItem::withAction("Quit", [] { Apps::requestQuit(); }, commandKey("q")));

    return menu;
}

// Nothing to route the standard commands to: with no responder chain, an Edit
// menu is the app's to build from its own commands. Empty, so the bar leaves
// it out.
Menu standardEditMenu()
{
    return Menu {"Edit"};
}
} // namespace eacp::Graphics
