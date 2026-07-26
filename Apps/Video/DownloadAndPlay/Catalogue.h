#pragma once

#include <eacp/Core/Core.h>

#include <Miro/Reflect.h>

namespace VideoDemo
{
// One clip on offer. The fields line up with the objects in Clips.json, which
// is what MIRO_REFLECT is for: the mapping is declared once here rather than
// written out again as parsing code.
struct Clip
{
    std::string name;
    std::string detail;
    std::string url;

    // What the clip is cached as, so the same download is not fetched twice.
    std::string fileName;

    MIRO_REFLECT(name, detail, url, fileName)
};

struct Catalogue
{
    eacp::Vector<Clip> clips;

    MIRO_REFLECT(clips)
};

// The catalogue read out of the embedded Clips.json, parsed once on first use.
// Empty if the resource is missing or malformed — the app still runs, it just
// has nothing to offer.
const Catalogue& catalogue();
} // namespace VideoDemo
