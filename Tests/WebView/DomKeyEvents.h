#pragma once

#include <eacp/WebView/WebView/JsStringLiteral.h>
#include <eacp/Core/Core.h>

#include <string>

// DOM KeyboardEvents a test dispatches inside the page, so key-events.js runs
// its whole path without OS key injection. keyCode is the DOM value, which is
// also the Win32 virtual key.
namespace DomKeyEvents
{
constexpr auto spaceVirtualKey = 32;
constexpr auto pageUpVirtualKey = 33;
constexpr auto pageDownVirtualKey = 34;
constexpr auto endVirtualKey = 35;
constexpr auto homeVirtualKey = 36;
constexpr auto leftArrowVirtualKey = 37;
constexpr auto upArrowVirtualKey = 38;
constexpr auto rightArrowVirtualKey = 39;
constexpr auto downArrowVirtualKey = 40;
constexpr auto tabVirtualKey = 9;
constexpr auto letterBVirtualKey = 66;
constexpr auto letterXVirtualKey = 88;
constexpr auto deadKeyVirtualKey = 222;

struct DomKeyPress
{
    std::string target = "window";
    std::string type = "keydown";
    std::string key;
    int keyCode = 0;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;
    bool repeat = false;
    bool isComposing = false;
};

inline std::string getJavaScriptBoolean(bool value)
{
    return value ? "true" : "false";
}

inline std::string getElementExpression(const std::string& elementId)
{
    return "document.getElementById(" + eacp::Graphics::jsStringLiteral(elementId)
           + ")";
}

// cancelable so a page's preventDefault() takes, as on a real key event.
inline std::string createDomKeyDispatchScript(const DomKeyPress& press)
{
    return press.target + ".dispatchEvent(new KeyboardEvent('" + press.type
           + "', {key: " + eacp::Graphics::jsStringLiteral(press.key)
           + ", keyCode: " + std::to_string(press.keyCode)
           + ", shiftKey: " + getJavaScriptBoolean(press.shift)
           + ", ctrlKey: " + getJavaScriptBoolean(press.control)
           + ", altKey: " + getJavaScriptBoolean(press.alt)
           + ", metaKey: " + getJavaScriptBoolean(press.meta)
           + ", repeat: " + getJavaScriptBoolean(press.repeat)
           + ", isComposing: " + getJavaScriptBoolean(press.isComposing)
           + ", bubbles: true, cancelable: true})); 'ok'";
}

struct DomNamedKey
{
    std::string key;
    int keyCode = 0;
};

inline const auto spaceKey = DomNamedKey {" ", spaceVirtualKey};
inline const auto tabKey = DomNamedKey {"Tab", tabVirtualKey};
inline const auto letterBKey = DomNamedKey {"b", letterBVirtualKey};
inline const auto letterXKey = DomNamedKey {"x", letterXVirtualKey};
inline const auto pageUpKey = DomNamedKey {"PageUp", pageUpVirtualKey};
inline const auto pageDownKey = DomNamedKey {"PageDown", pageDownVirtualKey};

inline DomKeyPress createDomKeyPress(const DomNamedKey& namedKey)
{
    auto press = DomKeyPress {};
    press.key = namedKey.key;
    press.keyCode = namedKey.keyCode;
    return press;
}

inline const eacp::Vector<DomNamedKey> domNavigationKeys = {
    pageUpKey,
    pageDownKey,
    {"Home", homeVirtualKey},
    {"End", endVirtualKey},
    {"ArrowLeft", leftArrowVirtualKey},
    {"ArrowUp", upArrowVirtualKey},
    {"ArrowRight", rightArrowVirtualKey},
    {"ArrowDown", downArrowVirtualKey}};
} // namespace DomKeyEvents
