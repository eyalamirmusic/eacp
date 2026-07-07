#include "IconContainers.h"

#include <eacp/Graphics/Image/Image.h>
#include <iostream>
#include <string>
#include <string_view>

using namespace eacp::Graphics;

namespace
{

std::string option(int argc, char** argv, std::string_view name)
{
    for (auto i = 1; i + 1 < argc; ++i)
        if (name == argv[i])
            return argv[i + 1];

    return {};
}

int usage()
{
    std::cerr << "usage: eacp-icon-tool --format icns|ico|iconset"
                 " --in <image.(png|jpeg)> --out <path>\n";
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    const auto format = option(argc, argv, "--format");
    const auto in = option(argc, argv, "--in");
    const auto out = option(argc, argv, "--out");

    if (format.empty() || in.empty() || out.empty())
        return usage();

    std::string error;
    const auto source = Image::load(in, &error);
    if (!source)
    {
        std::cerr << "eacp-icon-tool: cannot load '" << in << "': " << error << '\n';
        return 1;
    }

    try
    {
        if (format == "icns")
            Icons::writeIcns(source, out);
        else if (format == "ico")
            Icons::writeIco(source, out);
        else if (format == "iconset")
            Icons::writeIconset(source, out);
        else
        {
            std::cerr << "eacp-icon-tool: unknown format '" << format << "'\n";
            return usage();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "eacp-icon-tool: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
