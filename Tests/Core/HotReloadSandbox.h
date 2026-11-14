#pragma once

#include "Common.h"
#include "HarnessScript.h"
#include <eacp/Core/Plugins/HotReloadLoop.h>
#include <eacp/Core/Utils/TemporaryFile.h>

struct HotReloadSandbox
{
    HotReloadSandbox() { nano::check(eacp::Files::createDirectories(root.path())); }

    eacp::FilePath sources() const { return root.path() / "Sources"; }

    eacp::FilePath modulePath() const
    {
        auto extension = eacp::FilePath {EACP_RELOAD_FIXTURE_A}.extension();
        return root.path() / ("Module" + extension);
    }

    eacp::Plugins::HotReloadLoop::Options
        loopOptions(const HarnessScript& buildScript) const
    {
        auto options = eacp::Plugins::HotReloadLoop::Options {};
        options.sources.directory = sources();
        options.sources.watcher.pollInterval = eacp::Time::MS {20};
        options.sources.watcher.settle = eacp::Time::MS {50};
        options.buildCommand = buildScript.options();
        options.modulePath = modulePath();
        options.stagingDirectory = root.path() / "Staging";
        return options;
    }

    HarnessScript copyScript(const eacp::FilePath& fixture) const
    {
        return HarnessScript {}.copy(fixture, modulePath());
    }

    // The first build copies `first`, every later one `then`, the module's
    // presence being what tells them apart.
    HarnessScript copyInTurnScript(const eacp::FilePath& first,
                                   const eacp::FilePath& then) const
    {
        return HarnessScript {}
            .ifExists(modulePath())
            .copy(then, modulePath())
            .unlessExists(modulePath())
            .copy(first, modulePath());
    }

    void install(const eacp::FilePath& fixture) const
    {
        eacp::Files::removeFile(modulePath());
        nano::check(eacp::Files::copyFile(fixture, modulePath()));
    }

    // As copyInTurnScript, for a module installed before the first build: a
    // marker file the first build leaves tells them apart instead.
    HarnessScript copyEachBuildScript(const eacp::FilePath& first,
                                      const eacp::FilePath& then) const
    {
        auto marker = root.path() / "Built";

        return HarnessScript {}
            .ifExists(marker)
            .copy(then, modulePath())
            .unlessExists(marker)
            .copy(first, modulePath())
            .write(marker, "");
    }

    void touchSource(std::string_view contents) const
    {
        writeTestFile(eacp::toStdPath(sources() / "nested/Layout.cpp"), contents);
    }

    eacp::TemporaryFile root;
};
