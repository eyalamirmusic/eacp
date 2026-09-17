#pragma once

#include "LinuxWindowSystem-Linux.h"
#include "WaylandDisplay-Linux.h"

#include <memory>

// wl_data_device, behind the Clipboard::Backend hooks. Owned by WaylandDisplay,
// which offers the backend to LinuxWindowSystem as soon as the connection is
// up and takes it away again on the way down, so a build with no compositor
// keeps Core's empty answers.
//
// Wayland has no clipboard the way X11 has none either: the selection belongs
// to whichever client owns it, and a paste is a pipe the owner writes into. So
// a copy is a wl_data_source this process serves until something replaces it,
// and a paste blocks the message thread while the connection is pumped by hand.

namespace eacp::Graphics
{
struct WaylandDataOffer;

class WaylandClipboard
{
public:
    explicit WaylandClipboard(WaylandDisplay& displayToUse);
    ~WaylandClipboard();

    WaylandClipboard(const WaylandClipboard&) = delete;
    WaylandClipboard& operator=(const WaylandClipboard&) = delete;

    bool copyText(std::string_view text);
    bool copyFiles(const Vector<std::string>& paths);
    std::string getText();
    bool hasText();

private:
    friend struct WaylandClipboardDispatch;

    // The seat can arrive after the connection, so the device is made on the
    // first call that needs one.
    bool ensureDevice();

    // The first of `mimes` the current selection offers, and null when it
    // offers none of them or there is no selection at all.
    const std::string* pickMime(const Vector<std::string>& mimes) const;

    bool offerSelection(std::string data, const Vector<std::string>& mimes);
    std::string receiveSelection(const Vector<std::string>& mimes);

    void selectionChanged(wl_data_offer* offer);
    void offerCreated(wl_data_offer* offer);
    void offerRefused(wl_data_offer* offer);
    void sendSelection(int fd);
    void sourceCancelled(wl_data_source* cancelled);

    void destroySource();
    void destroyOffers();

    WaylandDisplay& display;

    wl_data_device* device = nullptr;

    Vector<std::unique_ptr<WaylandDataOffer>> pendingOffers;
    std::unique_ptr<WaylandDataOffer> selection;

    wl_data_source* source = nullptr;

    // Shared, because the thread that writes it may outlive the source: a
    // receiver that stopped reading is not allowed to hold up a copy.
    std::shared_ptr<const std::string> sourceData;
};
} // namespace eacp::Graphics
