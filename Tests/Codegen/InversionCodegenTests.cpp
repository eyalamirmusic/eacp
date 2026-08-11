// The inversion-driven codegen path must emit the same TS modules the
// static-init path (MIRO_EXPORT_COMMAND + EACP_EVENT) gives WebViewReactAnim.
// Clock below mirrors that app's surface, and the substrings are its output.

#include <Miro/Codegen.h>
#include <Miro/Reflect.h>
#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace Miro;
using namespace Miro::TypeExport;

// At file scope, not in an anonymous namespace, so the qualified name on the
// wire is "Tick" rather than "(anonymous namespace)::Tick".
struct Tick
{
    double angle = 0.0;

    MIRO_REFLECT(angle)
};

// A reflect() method instead of a free function plus an EACP_EVENT macro.
// Codegen never invokes getCurrentTick, so it needs no state.
class Clock
{
public:
    void reflect(ApiReflector& r)
    {
        r.command(&Clock::getCurrentTick, "getCurrentTick");
        r.event(&Clock::tick, "tick");
    }

    Tick getCurrentTick() const { return Tick {}; }

    Event<Tick> tick;
};

namespace
{
const EmittedFile* findFile(const EA::Vector<EmittedFile>& files,
                            std::string_view suffix)
{
    for (auto& f: files)
    {
        if (f.filename.size() >= suffix.size()
            && std::string_view {f.filename}.substr(f.filename.size()
                                                    - suffix.size())
                   == suffix)
            return &f;
    }
    return nullptr;
}

bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}
} // namespace

auto icTypesModule =
    test("Inversion: ts format matches WebViewReactAnim baseline") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"ts"});
    auto* ts = findFile(files, ".ts");

    check(ts != nullptr);
    check(contains(ts->contents, "export interface Tick"));
    check(contains(ts->contents, "angle:"));
    check(contains(ts->contents, "number"));
};

auto icBackendModule =
    test("Inversion: backend format matches WebViewReactAnim baseline") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"backend"});
    auto* backend = findFile(files, ".backend.ts");

    check(backend != nullptr);
    check(contains(backend->contents, "getCurrentTick: (): Promise<T.Tick>"));
    check(contains(backend->contents,
                   "invoke('getCurrentTick', {}) as Promise<T.Tick>"));
};

auto icBridgeModule =
    test("Inversion: bridge format emits the standard runtime") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"bridge"});
    auto* bridge = findFile(files, ".bridge.ts");

    check(bridge != nullptr);
    // The bridge runtime is static text, so only its arrival is checked.
    check(!bridge->contents.empty());
};

auto icEventsModule =
    test("Inversion: events format matches WebViewReactAnim baseline") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"events"});
    auto* events = findFile(files, ".events.ts");

    check(events != nullptr);
    check(contains(events->contents, "import type * as T from './schema';"));
    check(contains(events->contents, "export interface Events"));
    check(contains(events->contents, "'tick': T.Tick;"));
};

auto icHooksModule =
    test("Inversion: hooks format emits makeNativeEvent for push-only event") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);

    // tick is push-only (no getTick command), so it goes through
    // makeNativeEvent with toJSON(Tick{}) as the initial value.
    check(contains(hooks->contents, "export const useTick = makeNativeEvent"));
    check(contains(hooks->contents, "event: 'tick'"));
    check(contains(hooks->contents, "makeNativeEvent"));
    // The default payload JSON came through the DescribeReflector walk rather
    // than the static-init eventRegistry.
    check(contains(hooks->contents, "\"angle\":"));
};

auto icAllFormatsTogether = test(
    "Inversion: requesting all default formats produces every expected file") = []
{
    auto files = buildCodegen<Clock>(
        "schema",
        EA::Vector<std::string> {
            "ts", "backend", "ts-server", "bridge", "events", "hooks"});

    check(findFile(files, ".ts") != nullptr);
    check(findFile(files, ".backend.ts") != nullptr);
    check(findFile(files, ".handlers.ts") != nullptr);
    check(findFile(files, ".bridge.ts") != nullptr);
    check(findFile(files, ".events.ts") != nullptr);
    check(findFile(files, ".hooks.ts") != nullptr);
};

// r.use("clock", clock) puts every command and event of the sub on the wire as
// "clock.<name>", which hooks codegen has to project onto both a valid TS
// identifier and the matching nested backend access path.

struct SubTick
{
    double angle = 0.0;

    MIRO_REFLECT(angle)
};

class ClockSub
{
public:
    void reflect(ApiReflector& r)
    {
        r.command(&ClockSub::getTick, "getTick");
        r.event(&ClockSub::tick, "tick");
    }

    SubTick getTick() const { return SubTick {}; }

    Event<SubTick> tick;
};

class PingSub
{
public:
    void reflect(ApiReflector& r) { r.event(&PingSub::ping, "ping"); }

    Event<SubTick> ping;
};

class HostApi
{
public:
    void reflect(ApiReflector& r)
    {
        r.use("clock", clock);
        r.use("ping", ping);
    }

    ClockSub clock;
    PingSub ping;
};

auto icHooksSubApiBridgeStoreIdentifier =
    test("Inversion: sub-API event + matching get<Name> emits a valid TS hook "
         "identifier") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);

    // "useClock.tick" is a property access, a syntax error after export const.
    check(contains(hooks->contents, "export const useClockTick"));
    check(!contains(hooks->contents, "export const useClock.tick"));
};

auto icHooksSubApiBridgeStoreBackendPath = test(
    "Inversion: sub-API hooks reference backend via the nested namespace path") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);

    // backend.getClock.tick is what a naive "get" + capitalise on the dotted
    // wire name produces: a non-existent command and an invalid property chain.
    check(contains(hooks->contents, "fetch: backend.clock.getTick"));
    check(!contains(hooks->contents, "fetch: backend.getClock"));
};

auto icHooksSubApiBridgeStoreEventName =
    test("Inversion: sub-API hooks preserve the dotted wire name in event:") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);

    // The identifier collapses, but the channel name must stay dotted to match
    // what the bridge emits.
    check(contains(hooks->contents, "event: 'clock.tick'"));
};

auto icHooksSubApiPushOnlyIdentifier =
    test("Inversion: sub-API push-only event emits a valid TS hook identifier") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);

    // ping has no matching get command, so this takes the makeNativeEvent
    // fallback.
    check(contains(hooks->contents, "export const usePingPing = makeNativeEvent"));
    check(!contains(hooks->contents, "export const usePing.ping"));
    check(contains(hooks->contents, "event: 'ping.ping'"));
};

struct SubItem
{
    std::string id;
    std::string text;

    MIRO_REFLECT(id, text)
};

struct SubItemState
{
    std::vector<SubItem> items;

    MIRO_REFLECT(items)
};

class TodosSub
{
public:
    void reflect(ApiReflector& r)
    {
        r.command(&TodosSub::getChanged, "getChanged");
        r.keyedEvent(&TodosSub::changed, "changed", "items", "id");
    }

    SubItemState getChanged() const { return SubItemState {}; }

    Event<SubItemState> changed;
};

class KeyedHostApi
{
public:
    void reflect(ApiReflector& r) { r.use("todos", todos); }

    TodosSub todos;
};

auto icHooksSubApiKeyedIdentifiers =
    test("Inversion: sub-API keyed event emits valid TS identifiers for the store "
         "+ all-hook") = []
{
    auto files =
        buildCodegen<KeyedHostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);
    check(!contains(hooks->contents, "todos.changedStore"));
    check(contains(hooks->contents, "todosChangedStore = makeKeyedStore"));

    check(contains(hooks->contents, "export const useTodosChanged"));
    check(!contains(hooks->contents, "export const useTodos.changed"));
};

auto icBackendSubApiNests =
    test("Inversion: sub-API commands nest into the backend object tree") = []
{
    auto files =
        buildCodegen<HostApi>("schema", EA::Vector<std::string> {"backend"});
    auto* backend = findFile(files, ".backend.ts");

    check(backend != nullptr);
    check(contains(backend->contents, "clock: {"));
    check(contains(backend->contents, "getTick: (): Promise<T.SubTick>"));
    check(contains(backend->contents, "invoke('clock.getTick', {})"));

    // The pre-fix output emitted the wire name as a single dotted key, which is
    // invalid in an object literal.
    check(!contains(backend->contents, "clock.getTick: (): Promise"));
};

auto icHooksSubApiKeyedBackendPath = test(
    "Inversion: sub-API keyed event routes fetch via the nested backend path") = []
{
    auto files =
        buildCodegen<KeyedHostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);

    // 'todos.getChanged' is reached through the nested namespace object the
    // backend formatter emits.
    check(contains(hooks->contents, "fetch: backend.todos.getChanged"));
    check(!contains(hooks->contents, "fetch: backend.getTodos"));
};
