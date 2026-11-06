#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Graphics/Keyboard.h>
#include <Miro/Miro.h>

#include <optional>
#include <string>
#include <utility>

// The text formats the WebView2 backend reads and writes, kept free of any
// Windows type so every platform's test lane checks them.
namespace eacp::Graphics::detail
{
struct KeyVerdict
{
    bool isDown = false;
    bool consumed = false;
    int virtualKey = 0;
    ModifierKeys modifiers;
    bool isRepeat = false;
    std::string characters;
};

// The object key-events.js posts for every key event the page saw. keyCode is
// a Win32 virtual key and modifiers a bitmask (shift 1, ctrl 2, alt 4, meta 8).
struct KeyEventReport
{
    std::string kind;
    bool consumed = false;
    int keyCode = 0;
    int modifiers = 0;
    bool repeat = false;
    std::string key;

    MIRO_REFLECT(kind, consumed, keyCode, modifiers, repeat, key)
};

// Reads key-events.js's report. Empty when the text is not a JSON object or
// its kind is neither "down" nor "up".
std::optional<KeyVerdict> parseKeyVerdict(const std::string& json);

struct WebMessage
{
    std::string name;
    std::string body;
};

// Reads the {"name": "...", "body": ...} envelope the page posts. A string
// body is handed on unchanged, an object or array is printed back to JSON and
// any other body is empty, as on macOS. Empty when the text is not JSON.
std::optional<WebMessage> parseWebMessageEnvelope(const std::string& json);

// WebView2 returns every script result JSON-encoded, where macOS hands back a
// string raw: a JSON string is decoded one layer, anything else passes through.
std::string decodeScriptResult(const std::string& resultJson);

// The text before "://", or nothing when the URL has no scheme separator.
std::optional<std::string> getUrlScheme(const std::string& url);

// The reason phrase for a streaming response's status (200, 206 or 416).
std::string getStreamingStatusReason(int statusCode);

// The reason phrase for a ResourceProvider response: "OK" for 2xx, else "Error".
std::string getResourceStatusReason(int statusCode);

// Joins headers as the "Name: value" lines, separated by CRLF, that
// CreateWebResourceResponse takes.
std::string
    joinResponseHeaders(const Vector<std::pair<std::string, std::string>>& headers);

// The script that exposes a message handler as both window.<name> and
// window.webkit.messageHandlers.<name>, so page code posts the same way on
// WebView2 as on WebKit.
std::string buildMessageHandlerScript(const std::string& name);

// The script that tells window-controls.js whether the window is maximized.
std::string buildMaximizedStateScript(bool isMaximized);
} // namespace eacp::Graphics::detail
