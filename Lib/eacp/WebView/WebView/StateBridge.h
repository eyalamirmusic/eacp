#pragma once

#include "EventRegistry.h"

#include <Miro/Bridge.h>

namespace eacp::Graphics
{

// Process-wide auto-bind registry: EACP_STATE adds a binder at static init and
// every transport subscribes to all of them, unsubscribing when it dies.

using StateBinder = std::function<OwningPointer<EA::Listener>(Miro::Bridge&)>;

namespace Detail
{

// Inline so EACP_STATE works without linking eacp-webview, which the Miro
// codegen executable does not.
inline Vector<StateBinder>& stateBinderRegistry()
{
    static auto registry = Vector<StateBinder> {};
    return registry;
}

template <typename Store>
inline void registerStateBinder(Store& (*accessor)(), std::string eventName)
{
    stateBinderRegistry().add(
        [accessor, eventName = std::move(eventName)](
            Miro::Bridge& bridge) -> OwningPointer<EA::Listener>
        {
            auto& store = accessor();
            return makeOwned<EA::Listener>(
                store,
                [&store, &bridge, eventName]
                { bridge.emit(eventName, store.get()); },
                EA::Listener::Modes::TriggerOnEvent);
        });
}

} // namespace Detail

Vector<OwningPointer<EA::Listener>> attachStaticStateBinders(Miro::Bridge& bridge);

} // namespace eacp::Graphics

#define EACP_STATE_CAT2(a, b) a##b
#define EACP_STATE_CAT(a, b) EACP_STATE_CAT2(a, b)

// Re-emits accessor().get() under `eventName` whenever the store broadcasts.
// The store must expose an EA::Broadcaster and a `const T& get() const`, and
// trigger() after each mutation. Expands in a TU only, never a header.
#define EACP_STATE(T, accessor, eventName)                                          \
    namespace                                                                       \
    {                                                                               \
    [[maybe_unused]] const auto EACP_STATE_CAT(eacpState_, __LINE__) = []           \
    {                                                                               \
        ::eacp::Graphics::Detail::registerEvent<T>(#eventName);                     \
        ::eacp::Graphics::Detail::registerStateBinder(&(accessor), #eventName);     \
        return 0;                                                                   \
    }();                                                                            \
    }

// EACP_STATE plus keyed-collection metadata, so hooks codegen can emit
// per-id selector hooks:
//   EACP_KEYED_STATE(TodoState, todoStore, todos, items, id)
#define EACP_KEYED_STATE(T, accessor, eventName, collectionField, keyField)         \
    namespace                                                                       \
    {                                                                               \
    [[maybe_unused]] const auto EACP_STATE_CAT(eacpKeyedState_, __LINE__) = []      \
    {                                                                               \
        ::eacp::Graphics::Detail::registerKeyedEvent<T>(                            \
            #eventName, #collectionField, #keyField);                               \
        ::eacp::Graphics::Detail::registerStateBinder(&(accessor), #eventName);     \
        return 0;                                                                   \
    }();                                                                            \
    }

// Push-only event with no store and no auto-binder — the C++ side calls
// bridge.emit() itself. The type slot is variadic so templated types with
// commas survive macro expansion: EACP_EVENT(prices, std::map<K, V>)
#define EACP_EVENT(name, ...)                                                       \
    namespace                                                                       \
    {                                                                               \
    [[maybe_unused]] const auto EACP_STATE_CAT(eacpEvent_, __LINE__) = []           \
    {                                                                               \
        ::eacp::Graphics::Detail::registerEvent<__VA_ARGS__>(#name);                \
        return 0;                                                                   \
    }();                                                                            \
    }
