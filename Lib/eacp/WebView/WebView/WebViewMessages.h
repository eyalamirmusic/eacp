#pragma once

#include <eacp/Graphics/Graphics/Keyboard.h>

#include <optional>
#include <string>

// The page messages the native backends read, free of platform types so
// every lane tests them.
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
} // namespace eacp::Graphics::detail
