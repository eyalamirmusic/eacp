#pragma once

#include "Widgets.h"

#include <memory>
#include <optional>
#include <string>

namespace eacp::UI
{
// What a menu is drawn with. A value rather than a set of setters, for what
// Theme is one: a product has one look for every menu it opens, and that look
// is something to build once and hand to each of them.
//
// Every member starts out as defaultTheme() would draw it, and fromTheme makes
// the same mapping from a theme of the caller's own.
struct PopupMenuStyle
{
    static PopupMenuStyle fromTheme(const Theme& theme);

    Color background = defaultTheme().panel;
    Color outline = defaultTheme().outline;
    Color text = defaultTheme().text;
    Color disabledText = defaultTheme().dimText.withAlpha(0.55f);
    Color highlight = defaultTheme().accentDim;
    Color highlightedText = defaultTheme().text;
    Color separator = defaultTheme().outline;
    Color tick = defaultTheme().accent;
    Color headerText = defaultTheme().dimText;
    Color arrow = defaultTheme().dimText;

    // Empty draws in the host's face, which is what makes a menu look like the
    // rest of the tree it was opened over.
    std::optional<Font> font;

    // What a section header is told apart by, besides its colour.
    FontStyle headerStyle = FontStyle::Bold;

    float itemHeight = 24.f;
    float separatorHeight = 9.f;

    // The border between the panel's edge and its rows.
    float padding = 4.f;
    float cornerRadius = 5.f;
    float outlineThickness = 1.f;

    // What text is indented by from either side of its row, and the column the
    // tick sits in before it -- kept whether or not anything in the menu is
    // ticked, so a menu does not shift sideways when an item is.
    float textInset = 8.f;
    float tickWidth = 16.f;

    // The column a submenu's arrow sits in, and the gap between an item's text
    // and its shortcut.
    float arrowWidth = 16.f;
    float shortcutGap = 24.f;

    // The strips a panel taller than the tree gets at its top and bottom, which
    // scroll it while the pointer is over them.
    float scrollArrowHeight = 14.f;
};

enum class PopupMenuPlacement
{
    // Under the anchor, or over it when there is no room under and more room
    // over.
    Below,

    // The same, the other way round.
    Above,

    // Beside it, top edges aligned, on the left instead when there is no room
    // on the right. What a submenu does.
    Right
};

struct PopupMenuOptions
{
    // The panel is at least this wide, and otherwise as wide as its widest
    // item. A menu dropped from a control is usually given the control's width.
    float minimumWidth = 0.f;

    PopupMenuPlacement placement = PopupMenuPlacement::Below;

    PopupMenuStyle style;

    // How long the pointer rests on an item before its submenu opens, or on
    // another item before an open submenu closes -- long enough to cross the
    // rows between an item and the submenu it opened without closing it.
    double subMenuDelay = 0.2;
};

// A list of commands opened over the tree, and closed by choosing one or by
// clicking anywhere else.
//
// Not a window, for the reasons ComboBox's list is not one: the tree may be a
// plugin editor inside somebody else's window, and a second native surface is
// something its host does not move, scale or close with it. So an open menu is
// one component, added last to the root of the tree and covering all of it --
// which puts it above everything, sends it every click, and makes a click
// outside its panels a click it can answer by closing. That click is consumed
// rather than passed on to whatever is under it, as ComboBox's is: a click that
// closes a menu is not also a click on the form behind it.
//
// Its panels -- the menu and each submenu open from it -- are drawn by that one
// component, clipped to the tree's surface, and a panel taller than the room it
// has scrolls.
//
// The menu is a value. It can be built, copied and kept, and while it is open
// it is what owns the open panels: destroying it closes them, without a result.
class PopupMenu
{
public:
    using Style = PopupMenuStyle;
    using Placement = PopupMenuPlacement;
    using Options = PopupMenuOptions;

    struct Item
    {
        Item();
        ~Item();

        // Deep: a copied item has a copy of the submenu, so a menu built once can
        // be handed out any number of times.
        Item(const Item& other);
        Item& operator=(const Item& other);

        Item(Item&& other) noexcept;
        Item& operator=(Item&& other) noexcept;

        // Whether the keyboard and the pointer can land on it: enabled, and
        // neither a separator nor a header.
        bool canHighlight() const;

        bool hasSubMenu() const;

        std::string text;

        // What onResult is told when this item is chosen. Zero is what a closed
        // menu reports, so an item meant to be told apart needs another.
        int id = 0;

        bool enabled = true;
        bool ticked = false;
        bool isSeparator = false;
        bool isSectionHeader = false;

        // Drawn at the right edge, and only drawn: the menu binds no keys.
        std::string shortcutText;

        std::unique_ptr<PopupMenu> subMenu;

        // Run when the item is chosen, before onResult.
        std::function<void()> action;
    };

    PopupMenu();
    ~PopupMenu();

    // The items only. An open menu stays with the object that opened it, and a
    // menu moved from while open closes, without a result.
    PopupMenu(const PopupMenu& other);
    PopupMenu& operator=(const PopupMenu& other);
    PopupMenu(PopupMenu&& other) noexcept;
    PopupMenu& operator=(PopupMenu&& other) noexcept;

    // Each returns the item it added, for what the arguments do not cover -- a
    // shortcut, say. The reference lasts until the next item is added.
    Item&
        addItem(int id, std::string text, bool enabled = true, bool ticked = false);

    // An item that answers with its action rather than with an id: onResult is
    // told 0 for it.
    Item& addItem(std::string text,
                  std::function<void()> action,
                  bool enabled = true,
                  bool ticked = false);

    Item& addItem(Item item);

    Item& addSeparator();

    // A row naming the items under it. Disabled, so it is never highlighted or
    // chosen, and drawn apart from a disabled item (see Style::headerText).
    Item& addSectionHeader(std::string text);

    // Choosing an item of a submenu answers through this menu's onResult and
    // the item's own action; the submenu's onResult is not called.
    Item& addSubMenu(std::string text, PopupMenu subMenu, bool enabled = true);

    // Closes the menu first, without a result, since its rows are about to go.
    void clear();

    int getNumItems() const;
    bool isEmpty() const;
    const Item& getItem(int index) const;

    // Opens the menu against `anchorInParentSpace`, a rectangle in
    // `parentForTree`'s coordinates -- usually its own bounds, for a button
    // dropping a menu. A menu already open closes first, reporting 0. Refused,
    // and false, for an empty menu or a component not in a hosted tree.
    bool showAt(Component& parentForTree,
                Rect anchorInParentSpace,
                const Options& options = {});

    // At the pointer, for the event a component was just sent: a right-click
    // menu, its top-left corner where the press was.
    bool showAtMouse(Component& source,
                     const MouseEvent& event,
                     const Options& options = {});

    // Closes the menu as a click outside it would, reporting 0.
    void dismiss();

    bool isShowing() const;

    // Called once for every time the menu is shown: with the chosen item's id,
    // or 0 when it closed without one. Called last, after the panels are gone
    // and focus is back where it was, so it may show this menu again or destroy
    // it.
    std::function<void(int)> onResult = [](int) {};

    // What is open, in the root's coordinates, for a test and for a caller
    // placing something beside a menu. Depth 0 is the menu itself and each
    // depth after it the submenu open from the one before. Empty, or -1, for a
    // depth that is not open.
    int getNumOpenPanels() const;
    Rect getPanelBounds(int depth = 0) const;

    // Where the row is drawn, scrolled, whether or not that is inside the panel.
    Rect getItemBounds(int depth, int index) const;

    int getHighlightedIndex(int depth = 0) const;
    float getScrollPosition(int depth = 0) const;

private:
    class Overlay;

    // Every way the menu closes ends here, and in this order: the panels go,
    // focus goes back, the action runs, then onResult -- each from a copy, so
    // either may destroy this menu.
    void close(int result, std::function<void()> action, bool restoreFocus);

    // Closes without a word: for a menu being destroyed, moved from or cleared.
    void closeSilently();

    Vector<Item> items;
    std::unique_ptr<Overlay> overlay;
};
} // namespace eacp::UI
