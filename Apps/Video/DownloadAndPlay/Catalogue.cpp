#include "Catalogue.h"

#include <ResEmbed/ResEmbed.h>

namespace VideoDemo
{
namespace
{
Catalogue loadCatalogue()
{
    // Embedded at build time by res_embed_add; nothing to ship alongside.
    if (auto resource = ResEmbed::get("Clips.json", "Clips"))
        return Miro::createFromJSONString<Catalogue>(resource.toStringView());

    eacp::logMessage("Clips.json resource missing — the catalogue is empty.");
    return {};
}
} // namespace

const Catalogue& catalogue()
{
    static const Catalogue loaded = loadCatalogue();
    return loaded;
}
} // namespace VideoDemo
