#pragma once

#include <Miro/Miro.h>

#include <eacp/Core/Utils/Containers.h>

#include <functional>
#include <string>
#include <utility>

// Codegen-time metadata for bridge events declared via EACP_STATE /
// EACP_KEYED_STATE. At static-init time the macros append one EventEntry
// per declared state to the process-wide registry below; the eacp
// codegen formatters (events / hooks) read the registry to emit typed
// TS modules.
//
// Header-only on purpose: the registry needs to be visible from both
// the user's runtime build (where the macros expand) and the codegen
// executable (where the formatters read it). A function-local static
// keeps the storage in one place across either link line without
// requiring a dedicated .cpp file in both libraries.
//
// Runtime cost is one unused Vector<EventEntry> that gets populated at
// startup and never read. State delivery uses the binder registry in
// StateBridge.h; this one is purely a codegen channel.

namespace eacp::Graphics
{

// Aliases Miro's framework-neutral EventInfo so the static-init path
// and the inversion-driven Context::events can use the same struct
// without translation. Same field set the format functors expect.
using EventEntry = Miro::TypeExport::EventInfo;

namespace Detail
{

inline Vector<EventEntry>& eventRegistry()
{
    static auto registry = Vector<EventEntry> {};
    return registry;
}

template <typename T>
inline void registerEvent(const char* nameToUse)
{
    auto entry = EventEntry {};
    entry.name = nameToUse;
    entry.payloadTypeName = Miro::Detail::typeNameOf<T>();
    entry.payloadQualifiedName = Miro::Detail::qualifiedNameOf<T>();
    entry.defaultPayloadJson = [] { return Miro::toJSON(T {}); };
    eventRegistry().add(std::move(entry));
}

inline void markEventKeyed(const char* collectionField, const char* keyField)
{
    auto& registry = eventRegistry();
    if (registry.empty())
        return;

    auto& entry = registry.back();
    entry.isKeyed = true;
    entry.collectionField = collectionField;
    entry.keyField = keyField;
}

template <typename T>
inline void registerKeyedEvent(const char* nameToUse,
                               const char* collectionField,
                               const char* keyField)
{
    registerEvent<T>(nameToUse);
    markEventKeyed(collectionField, keyField);
}

} // namespace Detail
} // namespace eacp::Graphics
