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

// A launcher with no eacp of its own claims the copy in the library it links:
// that copy is the application, its product is the launcher, and its identity
// is still the library. It decided so in its first static initialiser, before
// the launcher's C runtime had started.
auto tClaimedCopyIsStandalone = test("ModuleRole/claimedCopyIsStandalone") = []
{
    auto report = runModuleRoleLauncher(EACP_MODULE_ROLE_LAUNCHER);

    check(report["linked.isDLL"] == "0");
    check(report["linked.isDLLAtLoad"] == "0");
    check(isSameModuleFile(report["linked.product"], EACP_MODULE_ROLE_LAUNCHER));
    check(isSameModuleFile(report["linked.copy"], EACP_MODULE_ROLE_LIBRARY_A));

    check(isSameModuleFile(report["linked.application"], EACP_MODULE_ROLE_LAUNCHER));
};

// The same library, linked the same way, with no claim: a guest.
auto tUnclaimedCopyIsDLL = test("ModuleRole/unclaimedLinkedCopyIsDLL") = []
{
    auto report = runModuleRoleLauncher(EACP_MODULE_ROLE_LAUNCHER_UNCLAIMED);

    check(report["linked.isDLL"] == "1");
    check(report["linked.isDLLAtLoad"] == "1");
    check(isSameModuleFile(report["linked.product"], EACP_MODULE_ROLE_LIBRARY_A));
    check(isSameModuleFile(report["linked.copy"], EACP_MODULE_ROLE_LIBRARY_A));
};

// The claim names one image. Another library's copy in the same process —
// a plugin the launcher loads — stays a guest.
auto tClaimLeavesOtherCopies = test("ModuleRole/claimLeavesOtherCopiesGuests") = []
{
    auto report = runModuleRoleLauncher(EACP_MODULE_ROLE_LAUNCHER,
                                        {EACP_MODULE_ROLE_LIBRARY_B});

    check(report["linked.isDLL"] == "0");
    check(report["loaded.isDLL"] == "1");
    check(isSameModuleFile(report["loaded.product"], EACP_MODULE_ROLE_LIBRARY_B));
    check(isSameModuleFile(report["loaded.copy"], EACP_MODULE_ROLE_LIBRARY_B));
};

// Loaded by this test binary, an eacp app that claims nothing: a plugin, while
// this binary's own copy is standalone.
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
