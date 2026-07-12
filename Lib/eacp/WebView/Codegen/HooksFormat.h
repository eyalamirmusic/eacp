#pragma once

#include "../WebView/EventRegistry.h"

#include <Miro/CommandExport/CommandEntry.h>
#include <Miro/TypeTree/TypeTree.h>

#include <span>

namespace eacp::Graphics::Codegen
{

// Emits a TypeScript module pre-wiring a React hook per registered
// event:
//   - keyed event + a get<Name> command → `makeKeyedStore` instance
//     re-exported as use<Name> / use<Item>Ids / use<Item>
//   - plain event + a get<Name> command → `makeBridgeStore`
//   - any other event → `makeNativeEvent`
//
// Each generated hook closes over the matching `backend` command and
// the event name, so consumers can call e.g. `const todos = useTodos();`
// without restating the wiring at every callsite. Initial values come
// from `EventEntry::defaultPayloadJson` (i.e. toJSON(T{})).
//
// commands is consulted only to detect the existence of a get<Name>
// twin; nothing about the command beyond the name is read. typeRoots
// must include every event payload's reflected TypeNode.
std::string
    formatHooksModule(std::span<Miro::TypeTree::TypeNode> typeRoots,
                      std::span<const Miro::CommandExport::CommandEntry> commands,
                      std::span<const EventEntry> events,
                      std::string_view baseName);

} // namespace eacp::Graphics::Codegen
