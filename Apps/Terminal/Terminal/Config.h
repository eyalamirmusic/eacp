#pragma once

#include "TermTypes.h"

#include <Miro/Reflect.h>

#include <string>
#include <vector>

namespace term
{
// User configuration, read from ~/.config/wim.json. Unknown keys are
// ignored and missing keys keep their defaults, so the file can be shared
// with other tools and grown over time.
struct AppConfig
{
    std::vector<std::string> searchDirs = {"~/projects", "~"};
    std::string font = "JetBrains Mono";
    float fontSize = 13.0f;
    std::string theme = "rosepine";

    MIRO_REFLECT(searchDirs, font, fontSize, theme)
};

AppConfig loadConfig();

Theme themeByName(const std::string& name);

// "~/x" -> "$HOME/x"
std::string expandHome(const std::string& path);
} // namespace term
