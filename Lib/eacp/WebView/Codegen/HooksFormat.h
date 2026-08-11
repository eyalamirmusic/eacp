#pragma once

#include "../WebView/EventRegistry.h"

#include <Miro/CommandExport/CommandEntry.h>
#include <Miro/TypeTree/TypeTree.h>

#include <span>

namespace eacp::Graphics::Codegen
{

// Emits a TypeScript module with one React hook per registered event.
// typeRoots must contain every event payload's TypeNode; commands is read
// only to detect whether a get<Name> twin exists.
std::string
    formatHooksModule(std::span<Miro::TypeTree::TypeNode> typeRoots,
                      std::span<const Miro::CommandExport::CommandEntry> commands,
                      std::span<const EventEntry> events,
                      std::string_view baseName);

} // namespace eacp::Graphics::Codegen
