#pragma once

#include <eacp/Core/Core.h>

#include <Miro/Reflect.h>

namespace VideoDemo
{
// The fields line up with the objects in Clips.json.
struct Clip
{
    std::string name;
    std::string detail;
    std::string url;

    std::string fileName;

    MIRO_REFLECT(name, detail, url, fileName)
};

struct Catalogue
{
    eacp::Vector<Clip> clips;

    MIRO_REFLECT(clips)
};

// Parsed once from the embedded Clips.json; empty if missing or malformed.
const Catalogue& catalogue();
} // namespace VideoDemo
