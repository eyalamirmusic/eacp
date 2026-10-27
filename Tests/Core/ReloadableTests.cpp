#include "HotReloadSandbox.h"
#include "ReloadableTestObject.h"
#include <eacp/Core/Plugins/Reloadable.h>

#include <array>
#include <optional>
#include <stdexcept>

using namespace nano;

namespace ReloadableTests
{
using eacp::FilePath;
using eacp::OwningPointer;
using eacp::Plugins::Reloadable;
using eacp::Plugins::ReloadableEntry;
using eacp::Plugins::ReloadableOptions;
using eacp::Plugins::ReloadableTable;
using eacp::Plugins::Detail::reloadableEntries;
using eacp::Plugins::Detail::ReloadableSetup;
using eacp::Time::MS;

using AnswerFunction = int();
using Object = ReloadableTestObject;

const auto answerOne = FilePath {EACP_RELOADABLE_FIXTURE_A};
const auto answerTwo = FilePath {EACP_RELOADABLE_FIXTURE_B};
const auto answerLong = FilePath {EACP_RELOADABLE_FIXTURE_MISMATCH};
const auto objectOne = FilePath {EACP_RELOADABLE_FIXTURE_OBJECT_A};
const auto objectTwo = FilePath {EACP_RELOADABLE_FIXTURE_OBJECT_B};
const auto noObject = FilePath {EACP_RELOADABLE_FIXTURE_NULL};
const auto throwingObject = FilePath {EACP_RELOADABLE_FIXTURE_THROW};
const auto answerAndObject = FilePath {EACP_RELOADABLE_FIXTURE_PAIR};
const auto noTable = FilePath {EACP_RELOAD_FIXTURE_C};

constexpr auto returnedNothing = "HotReload: factory returned nothing";
constexpr auto threwNoObject = "HotReload: factory threw: no object today";

int compiledInAnswer()
{
    return 0;
}

struct CompiledInObject final : Object
{
    ~CompiledInObject() override { markDestroyed(); }

    int value() const override { return -1; }
};

Object* makeCompiledInObject()
{
    return new CompiledInObject;
}

Object* makeNoCompiledInObject()
{
    return nullptr;
}

Object* throwInsteadOfAnObject()
{
    throw std::runtime_error("no object today");
}

const auto answerEntries = reloadableEntries(&compiledInAnswer);
const auto pairEntries = reloadableEntries(&compiledInAnswer, &makeCompiledInObject);
const auto nullEntries = reloadableEntries(&makeNoCompiledInObject);
const auto throwingEntries = reloadableEntries(&throwInsteadOfAnObject);

struct PlainSettings
{
    int width = 0;
};

PlainSettings* makePlainSettings()
{
    return new PlainSettings {640};
}

const auto plainEntries = reloadableEntries(&makePlainSettings);

template <typename F>
std::string spelled()
{
    using namespace eacp::Plugins::Detail;
    return readableSignature(signatureOf<F>());
}

template <std::size_t size>
ReloadableTable tableOf(const std::array<ReloadableEntry, size>& entries)
{
    return {entries.data(), static_cast<int>(size)};
}

struct Log
{
    std::function<void(std::string_view line)> sink()
    {
        auto add = [this](std::string_view line) { lines.add(std::string {line}); };
        return add;
    }

    bool contains(std::string_view wanted) const
    {
        for (const auto& line: lines)
            if (line == wanted)
                return true;

        return false;
    }

    int buildsFinished() const
    {
        auto count = 0;
        auto building = false;

        for (const auto& line: lines)
        {
            if (line.starts_with("HotReload: building "))
            {
                building = true;
                continue;
            }

            for (auto prefix: {"HotReload: generation ",
                               "HotReload: build failed: ",
                               "HotReload: load failed: "})
            {
                if (building && line.starts_with(prefix))
                {
                    ++count;
                    building = false;
                }
            }
        }

        return count;
    }

    bool waitForBuilds(int count) const
    {
        auto reached = [this, count] { return buildsFinished() >= count; };
        return eacp::Threads::runEventLoopUntil(reached, MS {10000});
    }

    std::string line(int index) const
    {
        return index < lines.size() ? lines[index] : std::string {};
    }

    eacp::Vector<std::string> lines;
};

ReloadableOptions
    optionsFor(const HotReloadSandbox& sandbox, const std::string& script, Log& log)
{
    auto options = ReloadableOptions {};
    options.loop = sandbox.loopOptions(script);
    options.log = log.sink();
    return options;
}

ReloadableSetup staticSetup(const ReloadableTable& table)
{
    return {table, false, {}};
}

ReloadableSetup devSetup()
{
    return {{}, true, {}};
}

std::string thrownBy(const std::function<void()>& action)
{
    try
    {
        action();
    }
    catch (const std::logic_error& e)
    {
        return e.what();
    }

    return {};
}

auto tStatic = test("Reloadable/staticModeUsesTheCompiledInTable") = []
{
    auto log = Log {};
    auto options = ReloadableOptions {};
    options.log = log.sink();

    auto reloadable = Reloadable {staticSetup(tableOf(pairEntries)), options};

    check(reloadable.function<AnswerFunction>() == &compiledInAnswer);
    check(reloadable.call<AnswerFunction>() == 0);
    check(reloadable.create<Object>()->value() == -1);
    check(reloadable.describeState() == "static");
    check(reloadable.getGeneration() == 0);
    check(log.lines.empty());
};

auto tStaticNothing = test("Reloadable/aNullOrThrowingFactoryGivesNothing") = []
{
    auto log = Log {};
    auto options = ReloadableOptions {};
    options.log = log.sink();

    auto returnsNull = Reloadable {staticSetup(tableOf(nullEntries)), options};
    auto throws = Reloadable {staticSetup(tableOf(throwingEntries)), options};

    check(returnsNull.create<Object>() == nullptr);
    check(throws.create<Object>() == nullptr);
    check(log.contains(returnedNothing));
    check(log.contains(threwNoObject));
};

auto tPlainStruct = test("Reloadable/createMakesAPlainStruct") = []
{
    auto reloadable = Reloadable {staticSetup(tableOf(plainEntries)), {}};

    auto settings = reloadable.create<PlainSettings>();

    check(settings != nullptr);
    check(settings->width == 640);
};

auto tUnknownType = test("Reloadable/anUnlistedTypeThrows") = []
{
    auto reloadable = Reloadable {staticSetup(tableOf(answerEntries)), {}};

    auto createObject = [&reloadable] { reloadable.create<Object>(); };
    auto fetchLong = [&reloadable] { reloadable.function<long()>(); };

    check(spelled<Object*()>().starts_with("ReloadableTestObject"));
    check(spelled<long()>().starts_with("long"));
    check(thrownBy(createObject)
          == "EACP_RELOADABLE lists no " + spelled<Object*()>());
    check(thrownBy(fetchLong) == "EACP_RELOADABLE lists no " + spelled<long()>());
};

auto tGenerationZero = test("Reloadable/generationZeroIsTheModule") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(answerAndObject);

    auto log = Log {};
    auto reloadable = Reloadable {devSetup(), optionsFor(sandbox, "true", log)};

    check(reloadable.getGeneration() == 0);
    check(reloadable.describeState() == "generation 0");
    check(reloadable.call<AnswerFunction>() == 7);
    check(reloadable.create<Object>()->value() == 7);
    check(log.line(1) == "HotReload: generation 0");
};

auto tFirstLoadFails = test("Reloadable/aFailedFirstLoadLeavesNothingLoaded") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");

    auto log = Log {};
    auto loadFailures = 0;
    auto options = optionsFor(sandbox, sandbox.copyScript(answerOne), log);
    auto countFailure = [&loadFailures](const std::string&) { ++loadFailures; };
    options.onLoadFailed = countFailure;

    auto reloadable = Reloadable {devSetup(), options};

    check(loadFailures == 1);
    check(log.line(1).starts_with("HotReload: load failed: no module at "));
    check(reloadable.describeState().starts_with("load failed: no module at "));
    check(reloadable.getGeneration() == 0);
    check(reloadable.function<AnswerFunction>() == nullptr);
    check(reloadable.call<AnswerFunction>() == 0);
    check(reloadable.create<Object>() == nullptr);

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));

    check(reloadable.getGeneration() == 0);
    check(reloadable.describeState() == "generation 0");
    check(reloadable.call<AnswerFunction>() == 1);
    check(log.contains("HotReload: generation 0"));
};

auto tCalledAtCreation = test("Reloadable/onReloadedRunsOnceAtCreation") = []
{
    auto reloads = 0;
    auto countReload = [&reloads] { ++reloads; };
    auto options = ReloadableOptions {};
    options.onReloaded = countReload;

    auto staticReloadable =
        Reloadable {staticSetup(tableOf(answerEntries)), options};
    check(reloads == 1);

    auto sandbox = HotReloadSandbox {};
    sandbox.install(answerOne);

    auto log = Log {};
    auto devOptions = optionsFor(sandbox, "true", log);
    devOptions.onReloaded = countReload;

    auto devReloadable = Reloadable {devSetup(), devOptions};
    check(reloads == 2);
    check(devReloadable.getGeneration() == 0);
    check(devReloadable.call<AnswerFunction>() == 1);
    check(log.lines.size() == 2);
};

auto tNotCalledAtCreation =
    test("Reloadable/onReloadedCanBeLeftUntilTheFirstReload") = []
{
    auto reloads = 0;
    auto countReload = [&reloads] { ++reloads; };
    auto options = ReloadableOptions {};
    options.onReloaded = countReload;
    options.callOnReloadedAtCreation = false;

    auto reloadable = Reloadable {staticSetup(tableOf(answerEntries)), options};

    check(reloads == 0);
    check(reloadable.call<AnswerFunction>() == 0);
};

auto tSwapsFunction = test("Reloadable/aFunctionSwapsAcrossGenerations") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(answerOne);

    auto log = Log {};
    auto reloads = 0;
    auto options =
        optionsFor(sandbox, sandbox.copyEachBuildScript(answerTwo, answerOne), log);
    auto countReload = [&reloads] { ++reloads; };
    options.onReloaded = countReload;

    auto reloadable = Reloadable {devSetup(), options};
    check(reloads == 1);
    check(reloadable.call<AnswerFunction>() == 1);

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));
    check(reloadable.function<AnswerFunction>()() == 2);
    check(reloadable.getGeneration() == 1);

    sandbox.touchSource("abc");
    check(log.waitForBuilds(2));
    check(reloadable.call<AnswerFunction>() == 1);
    check(reloadable.getGeneration() == 2);
    check(reloadable.describeState() == "generation 2");
    check(reloads == 3);
};

auto tIndependentObjects = test("Reloadable/createMakesIndependentObjects") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.install(objectOne);

    auto log = Log {};
    auto reloadable = Reloadable {devSetup(), optionsFor(sandbox, "true", log)};

    auto first = reloadable.create<Object>();
    auto second = reloadable.create<Object>();
    auto firstDestroyed = false;
    auto secondDestroyed = false;
    first->destroyed = &firstDestroyed;
    second->destroyed = &secondDestroyed;

    check(first.get() != second.get());
    check(first->value() == 1);
    check(second->value() == 1);

    first.reset();
    check(firstDestroyed);
    check(!secondDestroyed);
    check(second->value() == 1);
};

// The app's pattern: the holder of the objects first, the Reloadable last, and
// onReloaded the one place objects are made.
struct ObjectOwner
{
    explicit ObjectOwner(const ReloadableOptions& base)
        : reloadable {devSetup(), withReplacement(base)}
    {
    }

    ReloadableOptions withReplacement(const ReloadableOptions& base)
    {
        auto options = base;
        auto replace = [this] { replaceObject(); };
        options.onReloaded = replace;
        return options;
    }

    void replaceObject()
    {
        auto oldDestroyed = false;

        if (current != nullptr)
        {
            current->destroyed = &oldDestroyed;
            oldValues.add(current->value());
        }

        current = reloadable.create<Object>();

        if (oldDestroyed)
            ++destroyedInsideTheHook;

        newValues.add(current->value());
    }

    OwningPointer<Object> current;
    eacp::Vector<int> oldValues;
    eacp::Vector<int> newValues;
    int destroyedInsideTheHook = 0;
    Reloadable reloadable;
};

auto tReplacedInsideHook =
    test("Reloadable/objectsReplacedInOnReloadedDieWhileTheOldImageIsMapped") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(objectOne);

    auto log = Log {};
    auto owner = ObjectOwner {
        optionsFor(sandbox, sandbox.copyEachBuildScript(objectTwo, objectOne), log)};

    check(owner.newValues.size() == 1);
    check(owner.newValues[0] == 1);
    check(owner.oldValues.empty());

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));
    sandbox.touchSource("abc");
    check(log.waitForBuilds(2));

    check(owner.oldValues.size() == 2);
    check(owner.oldValues[0] == 1);
    check(owner.oldValues[1] == 2);
    check(owner.newValues.size() == 3);
    check(owner.newValues[1] == 2);
    check(owner.newValues[2] == 1);
    check(owner.destroyedInsideTheHook == 2);
    check(owner.current->value() == 1);
};

auto tOutlivesTheReloadable =
    test("Reloadable/anObjectDestroyedAfterTheReloadableIsSafe") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.install(objectOne);

    auto log = Log {};
    auto owner = std::optional<ObjectOwner> {};
    owner.emplace(optionsFor(sandbox, "true", log));

    check(owner->current->value() == 1);

    auto destroyed = false;
    owner->current->destroyed = &destroyed;

    auto destroyOwner = [&owner]
    {
        check(eacp::Threads::isEventLoopRunning());
        owner.reset();
    };

    eacp::Threads::runEventLoopFor(MS {20}, destroyOwner);

    check(destroyed);
};

auto tMissingFactory = test("Reloadable/aModuleMissingAFactoryIsRefused") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(answerAndObject);

    auto log = Log {};
    auto reloadable = Reloadable {
        devSetup(), optionsFor(sandbox, sandbox.copyScript(answerOne), log)};

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));

    check(reloadable.getGeneration() == 0);
    check(reloadable.call<AnswerFunction>() == 7);
    check(reloadable.describeState()
          == "load failed: the module lacks " + spelled<Object*()>());
};

auto tMismatch = test("Reloadable/aSignatureMismatchIsRefused") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(answerOne);

    auto log = Log {};
    auto loadFailures = eacp::Vector<std::string> {};
    auto options = optionsFor(sandbox, sandbox.copyScript(answerLong), log);
    auto keepFailure = [&loadFailures](const std::string& error)
    { loadFailures.add(error); };
    options.onLoadFailed = keepFailure;

    auto reloadable = Reloadable {devSetup(), options};

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));

    check(reloadable.call<AnswerFunction>() == 1);
    check(reloadable.getGeneration() == 0);
    check(loadFailures.size() == 1);
    check(reloadable.describeState().starts_with("load failed: signature mismatch"));
    check(loadFailures[0]
          == "signature mismatch: the module lacks " + spelled<AnswerFunction>()
                 + " and has " + spelled<long()>());
};

auto tNothing = test("Reloadable/aNullOrThrowingModuleFactoryGivesNothing") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(objectOne);

    auto log = Log {};
    auto script = sandbox.copyEachBuildScript(noObject, throwingObject);
    auto reloadable = Reloadable {devSetup(), optionsFor(sandbox, script, log)};

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));
    check(reloadable.create<Object>() == nullptr);
    check(log.contains(returnedNothing));

    sandbox.touchSource("abc");
    check(log.waitForBuilds(2));
    check(reloadable.getGeneration() == 2);
    check(reloadable.create<Object>() == nullptr);
    check(log.contains(threwNoObject));
};

auto tDefaultLog = test("Reloadable/theDefaultLogLines") = []
{
    auto sandbox = HotReloadSandbox {};
    sandbox.touchSource("a");
    sandbox.install(answerOne);

    auto log = Log {};
    auto reloadable = Reloadable {
        devSetup(),
        optionsFor(sandbox, sandbox.copyEachBuildScript(answerTwo, noTable), log)};

    sandbox.touchSource("ab");
    check(log.waitForBuilds(1));
    sandbox.touchSource("abc");
    check(log.waitForBuilds(2));

    auto building = "HotReload: building " + sandbox.modulePath().stem() + "…";

    check(log.lines.size() == 6);
    check(log.line(0) == "HotReload: watching " + sandbox.sources().str());
    check(log.line(1) == "HotReload: generation 0");
    check(log.line(2) == building);
    check(log.line(3) == "HotReload: generation 1");
    check(log.line(4) == building);
    check(log.line(5) == "HotReload: load failed: no eacpReloadableTable");
    check(reloadable.call<AnswerFunction>() == 2);
};
} // namespace ReloadableTests
