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
    check(entries.size() == 2, "one compiled model, its stamp and nothing else");
    check(countEndingWith(entries, ".mlmodelc") == 1);
    check(countEndingWith(entries, ".used") == 1);
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
    check(entries.size() == 2, "the losers deleted their own copies");
    check(countEndingWith(entries, ".mlmodelc") == 1);
    check(countEndingWith(entries, ".used") == 1);
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

// Eviction. Each use touches <hash>.used beside the model; a compile then
// trims the directory to the budget, oldest use first, sparing what a live
// Model holds and anything used in the last hour. Backdating the stamp and
// the directory stands in for the hour passing.
namespace
{
constexpr auto tinyBudget = std::uint64_t {1};

FilePath stampOf(const FilePath& compiled)
{
    auto text = compiled.str();
    return FilePath {text.substr(0, text.size() - std::string {".mlmodelc"}.size())
                     + ".used"};
}

std::filesystem::file_time_type modifiedAt(const FilePath& path)
{
    auto error = std::error_code {};
    return std::filesystem::last_write_time(toStdPath(path), error);
}

void backdateByHours(const FilePath& path, int hours)
{
    auto error = std::error_code {};
    std::filesystem::last_write_time(toStdPath(path),
                                     std::filesystem::file_time_type::clock::now()
                                         - std::chrono::hours {hours},
                                     error);
}

void makeOld(const FilePath& compiled)
{
    backdateByHours(compiled, 2);
    backdateByHours(stampOf(compiled), 2);
}

Options withBudget(Options options, std::uint64_t budget)
{
    options.cacheBudgetBytes = budget;
    return options;
}

FilePath compileAndRelease(const FilePath& cache, const std::string& version)
{
    auto model = Model {};
    auto result = model.load(TestPrograms::elementwiseChain(8, 16),
                             namedWeights(cache, version));
    check(result.ok, result.error);
    return model.compiledPath();
}

bool exists(const FilePath& path)
{
    return eacp::File {path}.exists();
}
} // namespace

auto tHitTouchesTheStamp = test("MLCache/aHitTouchesTheStamp") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("stamp");
    auto compiled = compileAndRelease(cache, "1");
    check(exists(stampOf(compiled)), "a compile stamps its model");

    makeOld(compiled);
    auto backdated = modifiedAt(stampOf(compiled));
    auto directoryTime = modifiedAt(compiled);

    auto model = Model {};
    check(model.load(TestPrograms::elementwiseChain(8, 16), namedWeights(cache, "1"))
              .ok);
    check(model.wasCacheHit());
    check(modifiedAt(stampOf(compiled)) - backdated > std::chrono::hours {1});
    check(modifiedAt(compiled) == directoryTime, "the model's own directory");
};

auto tMissEvictsTheOlder =
    test("MLCache/aMissBeyondTheBudgetEvictsTheOlderModelAndKeepsTheNewer") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("evict");
    auto older = compileAndRelease(cache, "1");
    makeOld(older);

    auto newer = Model {};
    auto result = newer.load(TestPrograms::elementwiseChain(8, 16),
                             withBudget(namedWeights(cache, "2"), tinyBudget));
    check(result.ok, result.error);
    check(!newer.wasCacheHit());

    check(!exists(older), "the older model was evicted");
    check(!exists(stampOf(older)), "and its stamp with it");
    check(exists(newer.compiledPath()), "the newer one is kept");

    auto entries = entriesOf(cache);
    check(entries.size() == 2, "one model and its stamp, no trash left");
    check(countEndingWith(entries, ".mlmodelc") == 1);
};

auto tHeldModelIsKept =
    test("MLCache/aModelThisProcessHoldsIsKeptEvenWhenOldest") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("held");
    auto held = Model {};
    check(held.load(TestPrograms::elementwiseChain(8, 16), namedWeights(cache, "1"))
              .ok);
    makeOld(held.compiledPath());

    auto newer = Model {};
    check(newer
              .load(TestPrograms::elementwiseChain(8, 16),
                    withBudget(namedWeights(cache, "2"), tinyBudget))
              .ok);

    check(exists(held.compiledPath()));
    check(countEndingWith(entriesOf(cache), ".mlmodelc") == 2);
};

auto tRecentlyUsedIsKept = test("MLCache/aModelUsedInTheLastHourIsKept") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("recent");
    auto recent = compileAndRelease(cache, "1");

    auto newer = Model {};
    check(newer
              .load(TestPrograms::elementwiseChain(8, 16),
                    withBudget(namedWeights(cache, "2"), tinyBudget))
              .ok);

    check(exists(recent), "another process may have just taken a hit on it");
    check(countEndingWith(entriesOf(cache), ".mlmodelc") == 2);
};

auto tZeroBudgetEvictsNothing = test("MLCache/aBudgetOfZeroEvictsNothing") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("unlimited");
    auto older = compileAndRelease(cache, "1");
    makeOld(older);

    auto newer = Model {};
    check(newer
              .load(TestPrograms::elementwiseChain(8, 16),
                    withBudget(namedWeights(cache, "2"), 0))
              .ok);

    check(exists(older));
    check(countEndingWith(entriesOf(cache), ".mlmodelc") == 2);
};

auto tOrphanedStampIsSwept =
    test("MLCache/aStaleStampWithNoModelIsSweptOnAMiss") = []
{
    if (!isSupported())
        return;

    auto cache = freshCacheDirectory("orphan");
    auto orphan = cache / "0123456789abcdef0123456789abcdef.used";
    check(Files::createDirectories(cache));
    Files::writeFile(orphan, eacp::Span<const std::uint8_t> {});
    backdateByHours(orphan, 2);

    compileAndRelease(cache, "1");
    check(!exists(orphan));
};

auto tDefaultIsAtTheCompanyLevel =
    test("MLCache/theDefaultDirectoryIsTheCompanysWhenOneIsNamed") = []
{
    auto shared = defaultCacheDirectory("Acme", "Tool");
    check(shared
          == FilePath::appCacheDirectory("Acme", "Tool").parentDirectory()
                 / "CoreML");
    check(shared == defaultCacheDirectory("Acme", "Other"), "one per vendor");

    check(defaultCacheDirectory("", "Tool")
          == FilePath::appCacheDirectory("", "Tool") / "CoreML");

    auto company = eacp::Platform::getCompanyName();
    auto expected = company.empty()
                        ? FilePath::appCacheDirectory() / "CoreML"
                        : FilePath::appCacheDirectory(company, {}) / "CoreML";
    check(defaultCacheDirectory() == expected);
};
