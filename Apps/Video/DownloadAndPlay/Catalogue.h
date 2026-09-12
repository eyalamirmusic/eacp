#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Network/OnlineResource/OnlineResource.h>

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

    // What the clip is kept as on disk: URL basenames like trailer.mp4 collide.
    std::string fileName;

    MIRO_REFLECT(name, detail, url, fileName)

    eacp::OnlineResource::Info resource() const { return {name, url, fileName}; }
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
