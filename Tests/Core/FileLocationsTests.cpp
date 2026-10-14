#include "Common.h"

using namespace nano;

namespace FileLocationsTests
{
using eacp::FilePath;

auto tSharedLocations = test("FileLocations/sharedLocationsAreAbsolute") = []
{
    auto common = FilePath::commonAppDataDirectory();
    auto applications = FilePath::applicationsDirectory();

    // An Android app sees only its own sandbox: there is nothing to name.
    if constexpr (eacp::Platform::isAndroid())
    {
        check(common.empty());
        check(applications.empty());
        return;
    }

    check(!common.empty());
    check(!applications.empty());
    check(common.isAbsolute());
    check(applications.isAbsolute());

    // An iOS sandbox may not be allowed to look at either.
    if constexpr (!eacp::Platform::isIOS())
    {
        check(eacp::Files::isDirectory(common));
        check(eacp::Files::isDirectory(applications));
    }

    if constexpr (eacp::Platform::isMac())
    {
        check(common.str() == "/Library");
        check(applications.str() == "/Applications");
    }
    else if constexpr (eacp::Platform::isLinux())
    {
        check(common.str() == "/opt");
        check(applications.str() == "/usr");
    }
};

auto tAppGroup = test("FileLocations/appGroupNeedsAnId") = []
{
    check(FilePath::appGroupContainerDirectory("").empty());

    // Elsewhere there are no app groups; on Apple platforms an unentitled
    // process may still be handed a path, which must then be a real one.
    auto unknown = FilePath::appGroupContainerDirectory("group.com.eacp.unknown");

    if constexpr (eacp::Platform::isApple())
        check(unknown.empty() || unknown.isAbsolute());
    else
        check(unknown.empty());
};

auto tApplicationPath = test("FileLocations/applicationPathIsThisBinary") = []
{
    auto application = eacp::Files::applicationPath();

    check(!application.empty());
    check(application.isAbsolute());
    check(eacp::Files::exists(application));

    // On Android the code lives in the app's .so, not in an executable.
    if constexpr (eacp::Platform::isAndroid())
        return;

    auto executable = eacp::Files::executablePath();

    // The test runner is a bare executable, not a bundle, so it is its own
    // application everywhere but iOS, where the bundle directory is.
    if constexpr (eacp::Platform::isIOS())
        check(application == executable.parentDirectory());
    else
        check(application.fileName() == executable.fileName());
};

auto tEnclosingBundle = test("FileLocations/enclosingBundleOfABundledBinary") = []
{
    using eacp::Files::enclosingBundle;

    check(enclosingBundle("/Applications/Foo.app/Contents/MacOS/Foo")
          == FilePath {"/Applications/Foo.app"});
    check(enclosingBundle("/Library/Audio/Plug-Ins/VST3/Foo.vst3/Contents/MacOS/Foo")
          == FilePath {"/Library/Audio/Plug-Ins/VST3/Foo.vst3"});
    check(enclosingBundle("/x/Foo.app/contents/macos/Foo")
          == FilePath {"/x/Foo.app"});

    check(enclosingBundle("/usr/local/bin/tool").empty());
    check(enclosingBundle("/x/NotContents/MacOS/Foo").empty());
    check(enclosingBundle("/x/Foo.app/Contents/Resources/Foo").empty());
    check(enclosingBundle("Foo").empty());
};
} // namespace FileLocationsTests
