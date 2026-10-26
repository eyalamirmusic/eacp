#pragma once

#include "Common.h"
#include <eacp/Core/Plugins/HotReloadLoop.h>
#include <eacp/Core/Utils/TemporaryFile.h>

inline std::string quotedPath(const eacp::FilePath& path)
{
    return "'" + path.str() + "'";
}

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
        loopOptions(const std::string& buildScript) const
    {
        auto options = eacp::Plugins::HotReloadLoop::Options {};
        options.sources.directory = sources();
        options.sources.watcher.pollInterval = eacp::Time::MS {20};
        options.sources.watcher.settle = eacp::Time::MS {50};
        options.buildCommand = shellCommand(buildScript);
        options.modulePath = modulePath();
        options.stagingDirectory = root.path() / "Staging";
        return options;
    }

    std::string copyScript(const eacp::FilePath& fixture) const
    {
        return "cp " + quotedPath(fixture) + " " + quotedPath(modulePath());
    }

    std::string copyInTurnScript(const eacp::FilePath& first,
                                 const eacp::FilePath& then) const
    {
        return inTurnScript(copyScript(first), copyScript(then));
    }

    std::string inTurnScript(const std::string& first, const std::string& then) const
    {
        return "if [ -f " + quotedPath(modulePath()) + " ]; then " + then + "; else "
               + first + "; fi";
    }

    void install(const eacp::FilePath& fixture) const
    {
        eacp::Files::removeFile(modulePath());
        nano::check(eacp::Files::copyFile(fixture, modulePath()));
    }

    std::string copyEachBuildScript(const eacp::FilePath& first,
                                    const eacp::FilePath& then) const
    {
        auto marker = quotedPath(root.path() / "Built");

        return "if [ -f " + marker + " ]; then " + copyScript(then) + "; else touch "
               + marker + "; " + copyScript(first) + "; fi";
    }

    void touchSource(std::string_view contents) const
    {
        writeTestFile(eacp::toStdPath(sources() / "nested/Layout.cpp"), contents);
    }

    eacp::TemporaryFile root;
};
