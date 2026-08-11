// Static-init registration of the React-hooks codegen format; the `events`
// format is owned by Miro upstream.

#include "HooksFormat.h"

#include "../WebView/EventRegistry.h"

#include <Miro/Codegen.h>

namespace
{

using Miro::TypeExport::Context;
using Miro::TypeExport::Format;
using Miro::TypeExport::registerFormat;

// Miro's Main.cpp leaves ctx.events empty, so fall back to the global registry.
std::span<const eacp::Graphics::EventEntry>
    eventsFor(const Miro::TypeExport::Context& ctx)
{
    if (!ctx.events.empty())
        return ctx.events;

    auto& global = eacp::Graphics::Detail::eventRegistry();
    return {global.data(), static_cast<std::size_t>(global.size())};
}

[[maybe_unused]] const auto hooksFormat = registerFormat(Format {
    "hooks",
    ".hooks.ts",
    [](const Context& ctx)
    {
        return eacp::Graphics::Codegen::formatHooksModule(
            ctx.typeRoots, ctx.commands, eventsFor(ctx), ctx.baseName);
    },
});

} // namespace
