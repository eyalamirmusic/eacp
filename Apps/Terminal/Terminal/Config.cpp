#include "Config.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/Core/Utils/Files.h>

namespace term
{
std::string expandHome(const std::string& path)
{
    if (path.empty() || path[0] != '~')
        return path;

    return eacp::FilePath::homeDirectory().str() + path.substr(1);
}

AppConfig loadConfig()
{
    auto config = AppConfig {};
    const auto path = eacp::FilePath::homeDirectory() / ".config" / "cowterm.json";
    const auto text = eacp::Files::readFile(path);

    if (!text.empty())
        Miro::fromJSONString(config, text);

    return config;
}

Theme themeByName(const std::string& name)
{
    if (name == "rosepine")
    {
        auto theme = Theme {};
        theme.background = 0x191724;
        theme.foreground = 0xe0def4;
        theme.cursor = 0xe0def4;
        theme.selection = 0x403d52;
        theme.paneBorder = 0x26233a;
        theme.paneBorderActive = 0x56526e;
        theme.ansi = {0x26233a,
                      0xeb6f92,
                      0x31748f,
                      0xf6c177,
                      0x9ccfd8,
                      0xc4a7e7,
                      0xebbcba,
                      0xe0def4,
                      0x6e6a86,
                      0xeb6f92,
                      0x31748f,
                      0xf6c177,
                      0x9ccfd8,
                      0xc4a7e7,
                      0xebbcba,
                      0xe0def4};
        return theme;
    }

    return Theme {};
}
} // namespace term
