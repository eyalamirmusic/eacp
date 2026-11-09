#include "IconContainers.h"

#include "../Image/Image.h"
#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Console.h>

using namespace eacp;
using namespace eacp::Graphics;

int main(int argc, char** argv)
{
    auto format = std::string {};
    auto in = FilePath {};
    auto out = FilePath {};

    auto cli = Args::Parser {
        "eacp-icon-tool",
        "Converts one PNG or JPEG image into a platform icon container."};
    cli.option("--format", format, "the container to write")
        .choices({"icns", "ico", "iconset"})
        .required();
    cli.option("--in", in, "the source PNG or JPEG image").hint("<file>").required();
    cli.option("--out", out, "where to write the icon").hint("<file>").required();
    cli.epilogue("An iconset is written as a directory at --out.\n"
                 "Exit codes: 0 written, 1 the image or the icon failed, "
                 "2 bad arguments.");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    std::string error;
    const auto source = Image::load(in, &error);
    if (!source)
    {
        Console::printError(Strings::concat(
            "eacp-icon-tool: cannot load '", in.str(), "': ", error));
        return 1;
    }

    try
    {
        if (format == "icns")
            Icons::writeIcns(source, out);
        else if (format == "ico")
            Icons::writeIco(source, out);
        else
            Icons::writeIconset(source, out);
    }
    catch (const std::exception& e)
    {
        Console::printError(Strings::concat("eacp-icon-tool: ", e.what()));
        return 1;
    }

    return 0;
}
