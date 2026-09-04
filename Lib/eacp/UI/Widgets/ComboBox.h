#pragma once

#include "Widgets.h"

#include <optional>
#include <string>

namespace eacp::UI
{
// A box showing one item out of a list, and the list itself while it is open.
//
// The list is not a window. A native popup is what a desktop toolkit reaches
// for, and it is the wrong shape here twice over: a component tree is one view,
// which may be a plugin editor embedded in somebody else's window, and a second
// native surface over it is a thing the host knows nothing about and does not
// move, scale or close with the editor. So the list is an ordinary component,
// added to the *root* of the tree rather than to the box -- a list is taller
// than the box that opens it, and every component paints through a Graphics
// already clipped to its own bounds, so a child of the box would be cut off at
// its edge.
//
// It covers the whole root, which is what makes a click anywhere outside the
// list dismiss it without a second mechanism for "clicked away", and it is
// clipped to the tree's own surface: a list opening near the bottom edge opens
// upward instead, since there is nowhere else for it to go.
class ComboBox final : public Component
{
public:
    explicit ComboBox(std::string placeholderToUse = {});
    ~ComboBox() override;

    void addItem(std::string text);
    void addItems(const Vector<std::string>& textsToAdd);

    // Empties the list and deselects, the selection being an index into what has
    // just gone.
    void clear();

    int getNumItems() const { return items.size(); }

    // Empty for an index that names no item, so a caller can ask about the
    // selection without testing it first.
    const std::string& getItemText(int index) const;

    // -1 means nothing is selected, and is the only value outside the list that
    // is accepted: stepping off either end does nothing rather than wrapping.
    //
    // Silent unless asked, the way every other setter in the tier is, so a
    // selection pushed in from whatever the box is attached to does not come
    // straight back out as a change.
    void setSelectedIndex(int index, bool notify = false);
    int getSelectedIndex() const { return selectedIndex; }

    // What the box draws: the selected item, or the placeholder while there is
    // no selection.
    const std::string& getText() const;

    void setPlaceholder(std::string newPlaceholder);

    void setAccentColour(const Color& colour);

    std::function<void(int)> onChange = [](int) {};

    void showPopup();
    void hidePopup();
    bool isPopupOpen() const { return popup.has_value(); }

    // Where the list is, in the root's coordinates, and empty while it is
    // closed -- so which way it opened is a question that can be asked rather
    // than one that has to be looked at.
    Rect getPopupBounds() const;

    void paint(Graphics& g) override;
    void resized() override;

    void mouseEnter(const MouseEvent&) override;
    void mouseExit(const MouseEvent&) override;
    void mouseDown(const MouseEvent& event) override;

    // Up and Down step the selection while the list is closed and move the
    // highlight while it is open; Return takes the highlighted item, Escape
    // puts the list away.
    bool keyDown(const KeyEvent& event) override;

    void focusLost() override;

private:
    // The open list. Lives only while the box is open, and is destroyed while it
    // is still in the tree -- see hidePopup, which is the one ordering rule this
    // widget has.
    struct Popup final : Component
    {
        explicit Popup(ComboBox& ownerToUse);

        // Sizes this component to the root it covers and puts the scroll
        // position back inside the list. Called when the list opens and again
        // whenever the box is resized.
        void layout();

        // Where the list is drawn: under the box, or over it when there is no
        // room under. Worked out on every ask rather than stored, so a box that
        // moves under an open list cannot leave it behind somewhere the box no
        // longer is.
        Rect getListBounds() const;

        // As tall as the box, within reason, so a list dropped from a large
        // control does not look like another widget's list.
        float getRowHeight() const;

        int rowAt(Point position) const;

        // Scrolls the row into view as well, which is what makes Up and Down
        // usable in a list taller than the tree.
        void setHighlightedRow(int row);
        void moveHighlight(int delta);
        int getHighlightedRow() const { return highlighted; }

        void setScrollPosition(float newOffset);
        float maximumScroll() const;

        void paint(Graphics& g) override;

        // Every point in the tree, and not merely every point in these bounds:
        // a click outside the list has to dismiss it wherever the root has
        // since grown to.
        bool hitTest(Point) const override { return true; }

        void mouseDown(const MouseEvent& event) override;
        void mouseMove(const MouseEvent& event) override;
        void mouseExit(const MouseEvent&) override;
        bool mouseWheelMove(const MouseEvent& event) override;

        ComboBox& owner;

        float scrollOffset = 0.f;
        int highlighted = -1;
    };

    // Selects and closes, in that order, so a listener acting on the change
    // finds a box that is already settled.
    void itemChosen(int index);

    // What an arrow key does to the selection: never off either end, and never
    // to nothing selected -- a keyboard step is a user moving through the list
    // rather than a caller clearing it.
    void stepSelection(int delta);

    Vector<std::string> items;
    std::string placeholder;
    Color accent = defaultTheme().accent;
    int selectedIndex = -1;

    std::optional<Popup> popup;
};
} // namespace eacp::UI
