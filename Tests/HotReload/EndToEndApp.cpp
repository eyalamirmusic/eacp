#include "EndToEndABI.h"

#include <eacp/Core/Core.h>
#include <eacp/Core/Plugins/HotReload.h>
#include <eacp/Core/Plugins/Reloadable.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/TemporaryFile.h>

#include <iostream>
#include <optional>
#include <string>

static_assert(eacp::Plugins::isHotReload(),
              "HotReloadEndToEnd is registered only for a dev-mode build");

int hotReloadEndToEndBase()
{
    return 40;
}

namespace
{
using namespace eacp;

const auto moduleTemplate = FilePath {EACP_END_TO_END_MODULE_TEMPLATE};
const auto moduleSource = FilePath {EACP_END_TO_END_MODULE_SOURCE};

// Well inside the test's ctest TIMEOUT of 900 s, so a slow runner fails here,
// naming the state the test was waiting for, rather than being killed.
constexpr auto overallTimeout = Time::MS {600'000};

struct Failed
{
    std::string what;
};

void require(bool condition, const std::string& what)
{
    if (!condition)
        throw Failed {what};
}

std::string replaced(std::string text, std::string_view from, std::string_view to)
{
    auto at = text.find(from);
    require(at != std::string::npos,
            "the template has no '" + std::string {from} + "'");
    return text.replace(at, from.size(), to);
}

std::string withStep(int step)
{
    return replaced(Files::readFile(moduleTemplate),
                    "step = 1;",
                    "step = " + std::to_string(step) + ";");
}

void writeSource(const std::string& text)
{
    Files::writeFileAtomically(moduleSource, std::string_view {text});
}

Processes::ProcessOptions buildCommand()
{
    return Plugins::hotReloadBuildCommand(Plugins::hotReloadTarget());
}

void buildNow()
{
    auto result = Processes::run(buildCommand());
    require(result.exited && result.exitCode == 0,
            "the module did not build:\n" + result.output + result.errorOutput);
}

struct EndToEnd
{
    EndToEnd()
    {
        // The module's builds keep the test's edits (RestoreModuleSource.cmake).
        setEnv("EACP_HOT_RELOAD_END_TO_END", "1");
        writeSource(withStep(1));
        buildNow();
        leaveACopyFromAnEarlierRun();
    }

    ~EndToEnd()
    {
        reloadable.reset();
        Threads::runEventLoopFor(Time::MS {100});
        writeSource(Files::readFile(moduleTemplate));

        if (!Processes::run(buildCommand()).exited)
            std::cerr << "HotReloadEndToEnd: could not rebuild the template\n";
    }

    FilePath leftover() const
    {
        auto module = FilePath {Plugins::hotReloadTarget().library};
        return staging.path() / (module.stem() + "-7-1-1") / module.fileName();
    }

    void leaveACopyFromAnEarlierRun() const
    {
        require(Files::createDirectories(leftover().parentDirectory()),
                "could not create " + leftover().parentDirectory().str());
        Files::writeFileAtomically(leftover(), std::string_view {"left over"});
    }

    Plugins::ReloadableOptions options() const
    {
        auto loop = Plugins::hotReloadOptions(Plugins::hotReloadTarget());
        loop.stagingDirectory = staging.path();
        loop.sources.watcher.pollInterval = Time::MS {50};
        loop.sources.watcher.settle = Time::MS {200};

        auto result = Plugins::ReloadableOptions {};
        result.loop = loop;
        return result;
    }

    void waitFor(const std::string& state)
    {
        auto reached = [this, &state]
        { return reloadable->describeState().starts_with(state); };

        require(Threads::runEventLoopUntil(reached, deadline.remaining()),
                "waited for '" + state + "', state is '"
                    + reloadable->describeState() + "'");
    }

    int answer() { return reloadable->call<EndToEndAnswer>(); }

    void run()
    {
        reloadable.emplace(options());

        require(!Files::exists(leftover().parentDirectory()),
                "the copy left by an earlier run was not swept");
        require(reloadable->describeState() == "generation 0",
                "first load: " + reloadable->describeState());
        require(answer() == 41, "generation 0 answered " + std::to_string(answer()));

        writeSource(withStep(2));
        waitFor("generation 1");
        require(answer() == 42, "generation 1 answered " + std::to_string(answer()));

        writeSource(withStep(3) + "\nthis is not C++;\n");
        waitFor("build failed");
        require(answer() == 42, "a failed build changed the answer");

        writeSource(replaced(withStep(3), "int answer()", "long answer()"));
        waitFor("load failed: signature mismatch");
        require(answer() == 42, "a refused module changed the answer");

        writeSource(withStep(4));
        waitFor("generation 2");
        require(answer() == 44, "generation 2 answered " + std::to_string(answer()));
    }

    Time::Deadline deadline {overallTimeout};
    TemporaryFile staging;
    std::optional<Plugins::Reloadable> reloadable;
};
} // namespace

int main()
{
    try
    {
        auto endToEnd = EndToEnd {};
        endToEnd.run();
        std::cout << "HotReloadEndToEnd: passed\n";
        return 0;
    }
    catch (const Failed& failure)
    {
        std::cerr << "HotReloadEndToEnd: " << failure.what << "\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << "HotReloadEndToEnd: " << exception.what() << "\n";
    }

    return 1;
}
