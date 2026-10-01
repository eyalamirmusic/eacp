#include "ModelTestCommon.h"

#include <sys/stat.h>
#include <thread>

using namespace nano;
using namespace ModelTests;

// The compile cache. Core ML keys its Neural Engine cache on the compiled
// model's directory, so a hit has to be the very directory the first load
// compiled: never recompiled, never replaced. The inode says it was not.
namespace
{
ino_t inodeOf(const FilePath& path)
{
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 ? info.st_ino : 0;
}

int countEndingWith(const Vector<std::string>& names, const std::string& suffix)
{
    auto endsWith = [&suffix](const std::string& name)
    {
        return name.size() >= suffix.size()
               && name.compare(name.size() - suffix.size(), suffix.size(), suffix)
                      == 0;
    };

    return names.countIf(endsWith);
}

Options namedWeights(const FilePath& cache, const std::string& version)
{
    auto options = optionsFor(ComputeUnits::cpu, cache);
    options.weightsName = "test-net";
    options.weightsVersion = version;
    return options;
}
} // namespace

auto tSecondLoadIsAHit = test("MLCache/aSecondLoadOfAPackageIsAHitInPlace") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("hit");
    auto package = TestPrograms::elementwiseChain(8, 16);

    auto first = Model {};
    auto firstResult = first.load(package, optionsFor(ComputeUnits::cpu, cache));
    check(firstResult.ok, firstResult.error);
    check(!first.wasCacheHit());

    auto compiled = first.compiledPath();
    auto inode = inodeOf(compiled);
    check(inode != 0);
    check(compiled.parentDirectory() == cache);

    auto second = Model {};
    auto secondResult = second.load(package, optionsFor(ComputeUnits::all, cache));
    check(secondResult.ok, secondResult.error);
    check(second.wasCacheHit());
    check(second.compiledPath() == compiled);
    check(inodeOf(compiled) == inode);

    auto entries = entriesOf(cache);
    check(entries.size() == 1, "one compiled model and nothing else");
    check(countEndingWith(entries, ".mlmodelc") == 1);
};

auto tChangedWeightVersionMisses =
    test("MLCache/aChangedWeightsVersionMissesAndTheOldCopyStays") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("version");
    auto package = TestPrograms::elementwiseChain(8, 16);

    auto model = Model {};
    check(model.load(package, namedWeights(cache, "1")).ok);
    check(!model.wasCacheHit());
    auto versionOne = model.compiledPath();

    check(model.load(package, namedWeights(cache, "1")).ok);
    check(model.wasCacheHit());

    check(model.load(package, namedWeights(cache, "2")).ok);
    check(!model.wasCacheHit());
    check(model.compiledPath() != versionOne);

    check(countEndingWith(entriesOf(cache), ".mlmodelc") == 2);
};

auto tNamedWeightsAreNotHashed =
    test("MLCache/namedWeightsAreTrustedAndAnUnnamedBlobIsHashed") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("named");
    auto one =
        TestPrograms::linearSoftmax(TestPrograms::linearSoftmaxWeights(4, 64, 1u));
    auto two =
        TestPrograms::linearSoftmax(TestPrograms::linearSoftmaxWeights(4, 64, 2u));

    check(one.model == two.model);
    check(one.weights != two.weights);

    auto model = Model {};
    check(model.load(one, namedWeights(cache, "1")).ok);
    check(model.load(two, namedWeights(cache, "1")).ok);
    check(model.wasCacheHit(), "the caller vouched for the name and version");

    check(model.load(one, optionsFor(ComputeUnits::cpu, cache)).ok);
    check(!model.wasCacheHit());
    check(model.load(two, optionsFor(ComputeUnits::cpu, cache)).ok);
    check(!model.wasCacheHit(), "a different blob is a different model");
};

auto tRaceLeavesOneDirectory =
    test("MLCache/loadsRacingForOnePackageLeaveOneDirectory") = []
{
    if (!isSupported())
        return;

    constexpr auto racers = 4;

    auto cache = freshCacheDirectory("race");
    auto package =
        TestPrograms::linearSoftmax(TestPrograms::linearSoftmaxWeights(64, 128));

    auto results = Array<Result, racers> {};
    auto paths = Array<FilePath, racers> {};
    auto threads = Vector<std::thread> {};

    for (auto index = 0; index < racers; ++index)
    {
        auto race = [&, index]
        {
            auto model = Model {};
            results[index] =
                model.load(package, optionsFor(ComputeUnits::cpu, cache));
            paths[index] = model.compiledPath();
        };

        threads.add(std::thread {race});
    }

    for (auto& thread: threads)
        thread.join();

    for (auto index = 0; index < racers; ++index)
    {
        check(results[index].ok, results[index].error);
        check(paths[index] == paths[0]);
    }

    auto entries = entriesOf(cache);
    check(entries.size() == 1, "the losers deleted their own copies");
    check(countEndingWith(entries, ".mlmodelc") == 1);
};

auto tPackageDirectoryGoesThroughTheCache =
    test("MLCache/anMlpackageDirectoryCompilesThroughTheCache") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("directory");
    auto packageDirectory =
        freshCacheDirectory("directory-source") / "net.mlpackage";
    auto package = TestPrograms::elementwiseChain(8, 16);
    check(package.write(packageDirectory));

    auto fromDirectory = Model {};
    auto loaded =
        fromDirectory.load(packageDirectory, optionsFor(ComputeUnits::cpu, cache));
    check(loaded.ok, loaded.error);
    check(!fromDirectory.wasCacheHit());

    auto fromBytes = Model {};
    check(fromBytes.load(package, optionsFor(ComputeUnits::cpu, cache)).ok);
    check(fromBytes.wasCacheHit(), "the same bytes, whichever way they came");
    check(fromBytes.compiledPath() == fromDirectory.compiledPath());

    auto compiled = Model {};
    check(
        compiled
            .load(fromDirectory.compiledPath(), optionsFor(ComputeUnits::cpu, cache))
            .ok);
    check(compiled.compiledPath() == fromDirectory.compiledPath());
};

auto tNotAPackageFails = test("MLCache/aDirectoryThatIsNoPackageFails") = []
{
    if (!isSupported())
        return;

    auto model = Model {};
    auto result = model.load(FilePath {"/nonexistent/eacp.mlpackage"});
    check(!result.ok);
    check(!result.error.empty());
    check(!model.isLoaded());
};
