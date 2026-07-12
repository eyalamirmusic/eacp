// End-to-end Phase D proof: the inversion-driven codegen path
// (codegenMain<Apis...>) produces the same TS modules WebViewReactAnim
// gets today from the static-init path (MIRO_EXPORT_COMMAND +
// EACP_EVENT macros), for an equivalent API class definition.
//
// The Clock class below mirrors WebViewReactAnim's surface:
//   - getCurrentTick() command returning a Tick
//   - tick event with Tick payload
//
// Each emitted file is checked against the same substrings the
// real app's web/src/generated/*.ts contains. If they match, the
// inversion path is wire-equivalent to the static-init path for
// this shape — no MIRO_EXPORT_COMMAND, no EACP_EVENT.

#include <Miro/Codegen.h>
#include <Miro/Reflect.h>
#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace Miro;
using namespace Miro::TypeExport;

// Matches WebViewReactAnim's Tick exactly — same name, same fields,
// same MIRO_REFLECT. Lives at file scope (not anonymous namespace) so
// the qualified name on the wire matches "Tick" rather than "(anonymous
// namespace)::Tick".
struct Tick
{
    double angle = 0.0;

    MIRO_REFLECT(angle)
};

// Same surface as WebViewReactAnim:
//   - one no-arg command returning Tick
//   - one push-only event of Tick
// Wrapped in a class with a reflect() method instead of a free fn +
// EACP_EVENT macro. No state to maintain; getCurrentTick returns
// default-constructed Tick for test purposes (codegen never invokes it).
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

// ---------- Miro-side formats ----------

auto icTypesModule =
    test("Inversion: ts format matches WebViewReactAnim baseline") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"ts"});
    auto* ts = findFile(files, ".ts");

    check(ts != nullptr);
    // Baseline: "export interface Tick { angle: number; }"
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
    // Baseline: "getCurrentTick: (): Promise<T.Tick> =>
    //               invoke('getCurrentTick', {}) as Promise<T.Tick>,"
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
    // The bridge runtime is static text — just confirm it landed.
    check(!bridge->contents.empty());
};

// ---------- EACP-side formats — the Phase D milestone ----------

auto icEventsModule =
    test("Inversion: events format matches WebViewReactAnim baseline") = []
{
    auto files = buildCodegen<Clock>("schema", EA::Vector<std::string> {"events"});
    auto* events = findFile(files, ".events.ts");

    check(events != nullptr);
    // Baseline:
    //   import type * as T from './schema';
    //   export interface Events
    //   {
    //       'tick': T.Tick;
    //   }
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
    // WebViewReactAnim's tick is push-only (no getTick command), so
    // hooks codegen wires it through makeNativeEvent with toJSON(Tick{})
    // as the initial value.
    check(contains(hooks->contents, "export const useTick = makeNativeEvent"));
    check(contains(hooks->contents, "event: 'tick'"));
    check(contains(hooks->contents, "makeNativeEvent"));
    // Default payload JSON came through Context::events from the
    // DescribeReflector walk — not the static-init eventRegistry.
    check(contains(hooks->contents, "\"angle\":"));
};

// ---------- Cross-cutting: all formats requested together ----------

auto icAllFormatsTogether = test(
    "Inversion: requesting all default formats produces every expected file") = []
{
    auto files = buildCodegen<Clock>(
        "schema",
        EA::Vector<std::string> {
            "ts", "backend", "ts-server", "bridge", "events", "hooks"});

    // Every requested format must produce one file.
    check(findFile(files, ".ts") != nullptr);
    check(findFile(files, ".backend.ts") != nullptr);
    check(findFile(files, ".handlers.ts") != nullptr);
    check(findFile(files, ".bridge.ts") != nullptr);
    check(findFile(files, ".events.ts") != nullptr);
    check(findFile(files, ".hooks.ts") != nullptr);
};

// ---------- Sub-APIs: r.use(key, sub) prefixes wire names with "key." ----------
//
// When an outer reflect() defers to a member via r.use("clock", clock),
// every command / event the sub declares lands on the wire as
// "clock.<name>". Hooks codegen has to project those dotted wire names
// onto:
//   - a valid TS identifier for the exported const (`useClockTick`,
//     not `useClock.tick` — a parser error)
//   - the matching backend access path (`backend.clock.getCurrentTick`,
//     not `backend.getClock.currentTick` — different command + invalid
//     property chain)
// These tests pin both projections. They fail today because HooksFormat
// concatenates `event.name` directly into both identifier slots.

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
    // Wire name is "clock.tick"; the exported const must be a single
    // identifier, not "useClock.tick" (a property access in TS, which is
    // a syntax error in an `export const ... =` position).
    check(contains(hooks->contents, "export const useClockTick"));
    check(!contains(hooks->contents, "export const useClock.tick"));
};

auto icHooksSubApiBridgeStoreBackendPath = test(
    "Inversion: sub-API hooks reference backend via the nested namespace path") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);
    // The wire event 'clock.tick' is paired with the wire command
    // 'clock.getTick' — same sub-API, conventional get<Name>. Hooks
    // must fetch via backend.clock.getTick (matching the nested object
    // the backend formatter builds from '.'-separated names), not via
    // backend.getClock.tick (a non-existent command derived by naive
    // "get" + capitalize on the dotted wire name).
    check(contains(hooks->contents, "fetch: backend.clock.getTick"));
    check(!contains(hooks->contents, "fetch: backend.getClock"));
};

auto icHooksSubApiBridgeStoreEventName =
    test("Inversion: sub-API hooks preserve the dotted wire name in event:") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);
    // The hook identifier collapses, but the runtime `event:` channel
    // name must still match what the bridge emits — i.e., the original
    // dotted wire name.
    check(contains(hooks->contents, "event: 'clock.tick'"));
};

auto icHooksSubApiPushOnlyIdentifier =
    test("Inversion: sub-API push-only event emits a valid TS hook identifier") = []
{
    auto files = buildCodegen<HostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);
    // ping has no matching get command; this exercises the
    // makeNativeEvent fallback. The exported const must still be a
    // single identifier.
    check(contains(hooks->contents, "export const usePingPing = makeNativeEvent"));
    check(!contains(hooks->contents, "export const usePing.ping"));
    check(contains(hooks->contents, "event: 'ping.ping'"));
};

// ---------- Sub-API + keyed event: store var, identifiers, and item hook ----------

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
    // Store variable identifier — must not contain a dot.
    check(!contains(hooks->contents, "todos.changedStore"));
    check(contains(hooks->contents, "todosChangedStore = makeKeyedStore"));

    // All-hook identifier — same rule.
    check(contains(hooks->contents, "export const useTodosChanged"));
    check(!contains(hooks->contents, "export const useTodos.changed"));
};

// ---------- Sub-APIs in the backend module: dotted names nest ----------

auto icBackendSubApiNests =
    test("Inversion: sub-API commands nest into the backend object tree") = []
{
    auto files =
        buildCodegen<HostApi>("schema", EA::Vector<std::string> {"backend"});
    auto* backend = findFile(files, ".backend.ts");

    check(backend != nullptr);
    // r.use("clock", clock); r.command(&ClockSub::getTick, "getTick") ->
    // wire name "clock.getTick" -> CommandExport tree:
    //   { clock: { getTick: (...) => invoke('clock.getTick', ...) } }
    check(contains(backend->contents, "clock: {"));
    check(contains(backend->contents, "getTick: (): Promise<T.SubTick>"));
    check(contains(backend->contents, "invoke('clock.getTick', {})"));
    // The pre-fix output emitted the wire name as a single dotted key
    // ("clock.getTick: ..."), which is invalid in an object literal.
    check(!contains(backend->contents, "clock.getTick: (): Promise"));
};

auto icHooksSubApiKeyedBackendPath = test(
    "Inversion: sub-API keyed event routes fetch via the nested backend path") = []
{
    auto files =
        buildCodegen<KeyedHostApi>("schema", EA::Vector<std::string> {"hooks"});
    auto* hooks = findFile(files, ".hooks.ts");

    check(hooks != nullptr);
    // Wire command is 'todos.getChanged' — accessed via the nested
    // namespace object the backend formatter emits.
    check(contains(hooks->contents, "fetch: backend.todos.getChanged"));
    check(!contains(hooks->contents, "fetch: backend.getTodos"));
};
