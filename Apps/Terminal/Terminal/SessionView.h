#pragma once

#include "Config.h"
#include "TerminalView.h"

#include <Miro/Reflect.h>

#include <memory>
#include <vector>

namespace term
{
// Flat, reflectable snapshot of a pane tree; `first`/`second` index into the
// same vector, node 0 is the root. Leaves carry the shell's directory.
struct SavedPane
{
    bool split = false;
    bool horizontal = false;
    float ratio = 0.5f;
    std::string cwd;

    // Names the pane's shell in the session daemon, so a restore adopts
    // the still-running process instead of spawning a new one.
    std::string shellId;
    int first = -1;
    int second = -1;

    MIRO_REFLECT(split, horizontal, ratio, cwd, shellId, first, second)
};

// One session's pane tree: every leaf is a live GPU terminal, splits carry
// an orientation (horizontal = side by side) and a ratio. Handles layout
// with gutters, splitting, closing, directional focus, keyboard resize and
// zoom. The chrome — gutters and the active-pane border — is CPU painted
// beneath the panes.
class SessionView final : public eacp::Graphics::View
{
public:
    SessionView(const AppConfig& configToUse, std::string fallbackDirToUse);
    ~SessionView() override;

    // Builds leaves from a snapshot; empty or malformed input yields a
    // single pane in the fallback directory. Call after wiring callbacks.
    void restore(const std::vector<SavedPane>& saved);
    std::vector<SavedPane> snapshot() const;

    TerminalView* activePane();
    const TerminalView* activePane() const;
    int paneCount() const;
    bool isClaudeAnywhere() const;

    void splitActive(bool horizontal);
    void closeActivePane();

    // Kills every pane's shell — closing a whole session on purpose, as
    // opposed to detaching at app teardown.
    void terminateAll();
    void focusDirection(char direction);
    void resizeActive(char direction, float cells);
    void toggleZoom();
    void cycleFocus();
    void focusActive();

    // Fired for every leaf created (restore and splits) so per-pane
    // callbacks (interceptKey, notify, title, cwd) get installed.
    std::function<void(TerminalView&)> onPaneCreated = [](TerminalView&) {};

    // Focus moved, or the tree changed shape.
    eacp::Callback onActivePaneChanged = [] {};

    // The last pane closed; the session is finished.
    eacp::Callback onEmpty = [] {};

    void resized() override;
    void paint(eacp::Graphics::Context& context) override;

private:
    struct Node
    {
        bool isLeaf() const { return view != nullptr; }

        std::unique_ptr<TerminalView> view;
        bool horizontal = false;
        float ratio = 0.5f;
        std::unique_ptr<Node> first;
        std::unique_ptr<Node> second;
        Node* parent = nullptr;
        eacp::Graphics::Rect bounds;
    };

    std::unique_ptr<Node> makeLeaf(const std::string& dir,
                                   const std::string& shellId = {});
    std::unique_ptr<Node>
        buildFromSaved(const std::vector<SavedPane>& saved, int index, int depth);
    void appendSnapshot(const Node& node, std::vector<SavedPane>& out) const;

    void layout();
    void layoutNode(Node& node, eacp::Graphics::Rect rect);
    void collectLeaves(Node* node, std::vector<Node*>& out) const;
    Node* leafFor(const TerminalView* view) const;
    void setActive(Node* leaf);
    void removeLeaf(Node* leaf);

    const AppConfig& config;
    std::string fallbackDir;
    Theme theme;
    std::unique_ptr<Node> root;
    Node* active = nullptr;
    Node* zoomed = nullptr;

    // Shell-exit removals are deferred to the next loop tick: removing the
    // leaf destroys the TerminalView whose callback is currently executing,
    // and a std::function must not die mid-invocation.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
} // namespace term
