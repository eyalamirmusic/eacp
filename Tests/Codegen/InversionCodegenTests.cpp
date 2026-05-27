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

#include <Miro/Miro.h>
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
            && std::string_view {f.filename}.substr(
                   f.filename.size() - suffix.size())
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

auto icTypesModule = test("Inversion: ts format matches WebViewReactAnim baseline") = []
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
    auto files =
        buildCodegen<Clock>("schema", EA::Vector<std::string> {"backend"});
    auto* backend = findFile(files, ".backend.ts");

    check(backend != nullptr);
    // Baseline: "getCurrentTick: (): Promise<T.Tick> =>
    //               invoke('getCurrentTick', {}) as Promise<T.Tick>,"
    check(contains(backend->contents,
                   "getCurrentTick: (): Promise<T.Tick>"));
    check(contains(backend->contents,
                   "invoke('getCurrentTick', {}) as Promise<T.Tick>"));
};

auto icBridgeModule =
    test("Inversion: bridge format emits the standard runtime") = []
{
    auto files =
        buildCodegen<Clock>("schema", EA::Vector<std::string> {"bridge"});
    auto* bridge = findFile(files, ".bridge.ts");

    check(bridge != nullptr);
    // The bridge runtime is static text — just confirm it landed.
    check(! bridge->contents.empty());
};

// ---------- EACP-side formats — the Phase D milestone ----------

auto icEventsModule =
    test("Inversion: events format matches WebViewReactAnim baseline") = []
{
    auto files =
        buildCodegen<Clock>("schema", EA::Vector<std::string> {"events"});
    auto* events = findFile(files, ".events.ts");

    check(events != nullptr);
    // Baseline:
    //   import type * as T from './schema';
    //   export interface ServerEvents
    //   {
    //       tick: T.Tick;
    //   }
    check(contains(events->contents, "import type * as T from './schema';"));
    check(contains(events->contents, "export interface ServerEvents"));
    check(contains(events->contents, "tick: T.Tick;"));
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

auto icAllFormatsTogether =
    test("Inversion: requesting all default formats produces every expected file") = []
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
