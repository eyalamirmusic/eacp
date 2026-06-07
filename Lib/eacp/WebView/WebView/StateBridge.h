#pragma once

#include "EventRegistry.h"

#include <Miro/Miro.h>

#include <eacp/Core/Utils/Containers.h>

#include <functional>
#include <string>
#include <utility>

namespace eacp::Graphics
{

// Auto-bind registry.
//
// EACP_STATE(...) registers a binder into a process-wide list at static init;
// each transport subscribes to all registered states at once and unsubscribes
// when it dies.
//
// A "store" is any user-defined class the macro can attach a Listener to (it
// derives from EA::Broadcaster or exposes `getBroadcaster()`) and that exposes
// a `get()` returning the payload by const-ref. The store owns its state, its
// mutators, and when to call trigger() — the macro only wires it to the bridge.

using StateBinder = std::function<OwningPointer<EA::Listener>(Miro::Bridge&)>;

namespace Detail
{

// Inline so static initializers in any TU using EACP_STATE can call it without
// linking the eacp-webview library (the Miro codegen executable doesn't).
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

// EACP_STATE — expose a user-defined store to the bridge.
//
// The user declares the store class and its accessor themselves; this
// macro only wires an auto-bind so any transport built later subscribes
// to the store and re-emits store.get() under `eventName` whenever the
// store's broadcaster fires.
//
// Usage (must be in a TU, not a header):
//   ParametersStore& parametersStore();   // user-defined
//   EACP_STATE(Parameters, parametersStore, parameters)
//
// The store class is responsible for:
//   - exposing a Broadcaster (inherit EA::Broadcaster or implement
//     `EA::Broadcaster& getBroadcaster()`)
//   - exposing `const T& get() const`
//   - calling trigger() on its broadcaster after each mutation
//   - providing whatever setters/mutators its callers need
//
// Two registries are populated on static init: the event registry
// (header-only, drives codegen — emits the Events type map and the
// React hooks module) and the binder registry (drives runtime — every
// transport auto-subscribes to broadcast changes to clients).
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

// EACP_KEYED_STATE — same as EACP_STATE but additionally declares the
// payload as a keyed collection (a vector field on the payload, each
// element identified by an id field). React-hooks codegen can read
// this metadata to emit `useXxx` / `useXxxIds` / `useXxxItem` hooks
// backed by `makeKeyedStore`, so the user gets per-id selector
// re-renders for free.
//
//   EACP_KEYED_STATE(TodoState, todoStore, todos,
//                    items,            // collection field on TodoState
//                    id)               // key field on TodoItem
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

// EACP_EVENT — push-only event with no C++-side store and no auto-
// binder. Use for patterns where the C++ side fires bridge.emit()
// directly (e.g. a timer or a callback) and the frontend only needs
// the typed `Events[name]` mapping for `backend.on(name, ...)`.
//
// Hooks codegen still emits a `use<Name>` factory backed by
// `makeNativeEvent` (initial value = toJSON(T{})). If the matching
// `get<Name>` command also exists, the formatter promotes it to
// `makeBridgeStore` automatically — but no get-command is required.
//
//   EACP_EVENT(tick, Tick)
//
// __VA_ARGS__ on the type slot so callers can pass templated types
// with commas without breaking macro expansion (e.g.
// `EACP_EVENT(prices, std::map<std::string, double>)`).
#define EACP_EVENT(name, ...)                                                       \
    namespace                                                                       \
    {                                                                               \
    [[maybe_unused]] const auto EACP_STATE_CAT(eacpEvent_, __LINE__) = []           \
    {                                                                               \
        ::eacp::Graphics::Detail::registerEvent<__VA_ARGS__>(#name);                \
        return 0;                                                                   \
    }();                                                                            \
    }
