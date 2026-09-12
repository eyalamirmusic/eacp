#pragma once

#include <eacp/Network/OnlineResource/OnlineResources.h>
#include <eacp/UI/Widgets/ListBox.h>
#include <eacp/UI/Widgets/Widgets.h>

#include <eacp/Core/Threads/Timer.h>

namespace eacp::UI
{
// The OnlineResources registry as a list: every resource declared or
// fetched, whether it is on disk and how big, and for one in flight a
// progress bar that keeps moving. The buttons under the list act on the
// selected row - fetch, cancel, delete its copy - and Clear deletes the
// registry's directory, cancelling anything still running first.
//
// Self-contained: it listens to the registry for state changes and polls
// progress while a transfer runs, so an app declares its resources, drops
// this in a window and is done. Nothing here starts a transfer the registry
// does not own.
class OnlineResourceMonitor final
    : public Component
    , private ListBoxModel
{
public:
    OnlineResourceMonitor();
    ~OnlineResourceMonitor() override;

    // A copy of what the list is showing, in row order.
    const Vector<OnlineResources::Entry>& getRows() const { return rows; }
    std::optional<OnlineResources::Entry> getSelectedEntry() const;

    // Re-reads the registry. Called for you on every change; here for a test
    // that wants the rows without pumping the loop.
    void refresh();

    void fetchSelected();
    void cancelSelected();
    void removeSelected();
    void clearAll();

    void resized() override;

private:
    int getNumRows() override { return rows.size(); }
    void paintRow(Graphics& g, int row, const Rect& bounds, bool selected) override;
    void selectedRowChanged(int row) override;
    void rowDoubleClicked(int row) override;

    void updateButtons();
    void updateSummary();
    void updatePolling();
    void finishPendingClear();

    Vector<OnlineResources::Entry> rows;
    OnlineResources::ListenerId listenerId = 0;
    OwningPointer<Threads::Timer> poll;
    bool clearPending = false;

    Label title;
    Label location;
    Label summary;
    ListBox list;
    Button fetchButton {"Fetch"};
    Button cancelButton {"Cancel"};
    Button removeButton {"Delete copy"};
    Button clearButton {"Clear all"};
};
} // namespace eacp::UI
