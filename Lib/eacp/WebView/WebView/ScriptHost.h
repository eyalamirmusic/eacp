#pragma once

#include "../Common.h"

namespace eacp::Graphics
{
// A single file the page can hand to the native drag-out. `path` is the
// absolute on-disk path the OS copies on drop; `name` is the display label.
struct DraggableFile
{
    std::string path;
    std::string name;

    MIRO_REFLECT(path, name)
};

// Payload of the built-in `armFileDrag` bridge command. The page sends
// `{ files: [{ path, name }, ...] }` and Miro deserializes it into this
// type. Multiple files start a single multi-file drag session.
struct DraggableFileList
{
    Vector<DraggableFile> files;

    MIRO_REFLECT(files)
};

// The whole of what WebViewBridge asks of the thing it talks to: a page it
// can inject a script into, a named channel that page posts strings back
// over, and a way to evaluate one more script against it. Graphics::WebView
// implements it over WKWebView / WebView2; anything else that runs a script
// engine against a document -- an engine that renders and scripts HTML
// itself, a test double -- implements the same five members and gets the
// bridge, the typed commands and the EACP_STATE broadcasts unchanged.
//
// Every member is called on the main thread. The contract each one owes,
// which is what the platform WebViews already do and what a new host has to
// match for a page to behave the same over both:
//
//  - addUserScript runs `source` in the page's own global scope for every
//    document this host loads, including the ones it navigates to later.
//    At document start that is before any of the page's own markup is
//    parsed, so `window.eacp` exists before the first inline <script>.
//  - addScriptMessageHandler routes a page's posts on `name` to `handler`,
//    replacing whatever was routed there before. The body is a raw string:
//    a page that posts an object sends its JSON, and a page that posts a
//    bare number or boolean sends nothing at all.
//  - removeScriptMessageHandler stops that routing. The page may go on
//    posting; the messages are dropped.
//  - evaluateJavaScript runs `script` against the current document and
//    discards its value. A script issued before there is a document to run
//    it against is queued or dropped, never an error.
//  - armFileDrag hands the next mouse gesture to the OS as a file drag.
//    A host with no native drag leaves the default, and the built-in
//    `armFileDrag` command still resolves rather than failing.
class ScriptHost
{
public:
    using MessageFunc = std::function<void(const std::string& message)>;

    virtual ~ScriptHost() = default;

    virtual void addUserScript(const std::string& source, bool atDocumentStart) = 0;

    virtual void addScriptMessageHandler(const std::string& name,
                                         const MessageFunc& handler) = 0;

    virtual void removeScriptMessageHandler(const std::string& name) = 0;

    virtual void evaluateJavaScript(const std::string& script) = 0;

    virtual void armFileDrag(const Vector<std::string>&) {}
};

} // namespace eacp::Graphics
