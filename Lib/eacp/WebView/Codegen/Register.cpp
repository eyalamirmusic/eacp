// Static-init registration of eacp's WebView codegen formats with the
// Miro type-export runner.
//
// This TU lives in the eacp-webview-codegen OBJECT library. The
// library is spliced into ${TARGET}Schema_Codegen by
// eacp_add_webview_app, so the initializers below fire whenever an
// app's codegen executable launches.
//
// The `events` format is owned by Miro upstream; only the React-hooks
// format is registered here.

#include "HooksFormat.h"

#include "../WebView/EventRegistry.h"

#include <Miro/Codegen.h>

namespace
{

using Miro::TypeExport::Context;
using Miro::TypeExport::Format;
using Miro::TypeExport::registerFormat;

// Source-agnostic event view: prefer ctx.events (the inversion path's
// DescribeReflector walk filled it) and fall back to the static-init
// global eventRegistry when ctx.events is empty (Miro Main.cpp doesn't
// know about events, so it leaves them empty for downstream resolvers).
// EventEntry is a Miro::EventInfo alias, so both sides feed the same
// formatter signature.
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
