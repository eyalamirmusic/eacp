#include "Common.h"
#include <eacp/Core/Plugins/DynamicLibrary.h>
#include <eacp/Core/Plugins/ModuleInfo.h>

#include <map>
#include <sstream>

using namespace nano;
using namespace eacp;
namespace fs = std::filesystem;

namespace
{
using ModuleRoleReport = std::map<std::string, std::string>;

ModuleRoleReport parseModuleRoleReport(const std::string& output)
{
    auto report = ModuleRoleReport {};
    auto lines = std::istringstream {output};

    for (auto line = std::string {}; std::getline(lines, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (auto equals = line.find('='); equals != std::string::npos)
            report[line.substr(0, equals)] = line.substr(equals + 1);
    }

    return report;
}

ModuleRoleReport runModuleRoleLauncher(const std::string& launcher,
                                       Vector<std::string> arguments = {})
{
    auto options = Processes::ProcessOptions {};
    options.executable = launcher;
    options.arguments = std::move(arguments);

    const auto result = Processes::run(std::move(options));

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);

    return parseModuleRoleReport(result.output);
}

bool isSameModuleFile(const std::string& reported, const std::string& expected)
{
    auto ec = std::error_code {};
    return fs::equivalent(toStdPath(FilePath {reported}), fs::path {expected}, ec);
}
} // namespace

auto tClaimedCopyIsStandalone = test("ModuleRole/claimedCopyIsStandalone") = []
{
    auto report = runModuleRoleLauncher(EACP_MODULE_ROLE_LAUNCHER);

    check(report["linked.isDLL"] == "0");
    check(report["linked.isDLLAtLoad"] == "0");
    check(isSameModuleFile(report["linked.product"], EACP_MODULE_ROLE_LAUNCHER));
    check(isSameModuleFile(report["linked.copy"], EACP_MODULE_ROLE_LIBRARY_A));
    check(isSameModuleFile(report["linked.application"], EACP_MODULE_ROLE_LAUNCHER));
};

auto tUnclaimedCopyIsDLL = test("ModuleRole/unclaimedLinkedCopyIsDLL") = []
{
    auto report = runModuleRoleLauncher(EACP_MODULE_ROLE_LAUNCHER_UNCLAIMED);

    check(report["linked.isDLL"] == "1");
    check(report["linked.isDLLAtLoad"] == "1");
    check(isSameModuleFile(report["linked.product"], EACP_MODULE_ROLE_LIBRARY_A));
    check(isSameModuleFile(report["linked.copy"], EACP_MODULE_ROLE_LIBRARY_A));
};

auto tClaimLeavesOtherCopies = test("ModuleRole/claimLeavesOtherCopiesGuests") = []
{
    auto report = runModuleRoleLauncher(EACP_MODULE_ROLE_LAUNCHER,
                                        {EACP_MODULE_ROLE_LIBRARY_B});

    check(report["linked.isDLL"] == "0");
    check(report["loaded.isDLL"] == "1");
    check(isSameModuleFile(report["loaded.product"], EACP_MODULE_ROLE_LIBRARY_B));
    check(isSameModuleFile(report["loaded.copy"], EACP_MODULE_ROLE_LIBRARY_B));
};

auto tLoadedCopyIsDLL = test("ModuleRole/loadedByAnUnclaimingAppIsDLL") = []
{
    auto library = Plugins::DynamicLibrary {FilePath {EACP_MODULE_ROLE_LIBRARY_A}};
    check(library.isOpen());

    auto isDLL = library.findFunction<int (*)()>("eacpModuleRoleIsDLL");
    auto product =
        library.findFunction<const char* (*) ()>("eacpModuleRoleProductPath");
    check(isDLL != nullptr && product != nullptr);

    check(isDLL() == 1);
    check(isSameModuleFile(product(), EACP_MODULE_ROLE_LIBRARY_A));
    check(Platform::isStandalone());
    check(Plugins::getProductModulePath() == Plugins::getCurrentModulePath());
};
