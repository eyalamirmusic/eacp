#pragma once

#include "TermTypes.h"

#include <Miro/Reflect.h>

#include <string>
#include <vector>

namespace term
{
// One Ctrl+A leader binding: `key` is the character typed after the
// prefix. Exactly one action should be set — `send` types text into the
// active pane ("\n" presses Enter, tmux send-keys), `popup` runs a command
// in a full-window popup over the session (tmux display-popup -E).
struct KeyBinding
{
    std::string key;
    std::string send;
    std::string popup;

    MIRO_REFLECT(key, send, popup)
};

// User configuration, read from ~/.config/wim.json. Unknown keys are
// ignored and missing keys keep their defaults, so the file can be shared
// with other tools and grown over time.
struct AppConfig
{
    std::vector<std::string> searchDirs = {"~/projects", "~"};
    std::string font = "JetBrains Mono";
    float fontSize = 13.0f;
    std::string theme = "rosepine";

    // Config bindings run before the built-in leader table, so a binding
    // here can also re-purpose a built-in key.
    std::vector<KeyBinding> bindings = {{.key = "u", .send = "cd ..\n"},
                                        {.key = "n", .send = "nvim .\n"}};

    MIRO_REFLECT(searchDirs, font, fontSize, theme, bindings)
};

AppConfig loadConfig();

Theme themeByName(const std::string& name);

// "~/x" -> "$HOME/x"
std::string expandHome(const std::string& path);
} // namespace term
