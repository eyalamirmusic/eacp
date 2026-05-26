#pragma once

#include <eacp/Network/HTTPServer/HttpServer.h>
#include <eacp/WebView/WebView.h>

#include <ea_data_structures/Pointers/OwningPointer.h>

namespace eacp::Graphics
{
// Loopback HTTP control surface that lets an external agent drive the WebView
// for autonomous QA: POST /evaluate-javascript runs arbitrary JS in the page
// (list elements, click, type, navigate — see the injected window.eacpQA
// helper), GET /screenshot returns a PNG of the rendered content.
//
// Debug-only. Bound to 127.0.0.1. Never start this in a shipping release —
// /evaluate-javascript is remote code execution against the page by design.
// Use startIfEnabled() to gate on the EACP_DEBUG_CONTROL env var.
//
// The server runs in ThreadPool mode so a request handler can block on the
// async WebView callbacks (evaluateJavaScript / captureSnapshot hop their
// results onto the main event loop) without deadlocking that loop.
class DebugControl
{
public:
    static constexpr int defaultPort = 7333;

    DebugControl(WebView& webViewToUse, int portToUse = defaultPort);

    DebugControl(const DebugControl&) = delete;
    DebugControl& operator=(const DebugControl&) = delete;

    // Injects window.eacpQA and binds the server. Returns false if the port
    // could not be bound.
    bool start();

    // Constructs + start()s only when EACP_DEBUG_CONTROL is set in the
    // environment; otherwise returns null. Call once from the app after the
    // WebView exists.
    static EA::OwningPointer<DebugControl> startIfEnabled(WebView& webView);

private:
    eacp::HTTP::Response handleEval(const eacp::HTTP::Request& request);
    eacp::HTTP::Response handleScreenshot(const eacp::HTTP::Request& request);

    WebView& webView;
    int port;
    eacp::HTTP::Server server;
};
} // namespace eacp::Graphics
