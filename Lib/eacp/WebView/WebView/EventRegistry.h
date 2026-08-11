#pragma once

#include <Miro/Reflect.h>
#include <Miro/TypeExport/Context.h>

#include <eacp/Core/Core.h>

// Codegen-only metadata for EACP_STATE events; runtime delivery goes through
// the binder registry in StateBridge.h instead. Header-only, because both the
// runtime build and the codegen executable need the same registry.

namespace eacp::Graphics
{

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
