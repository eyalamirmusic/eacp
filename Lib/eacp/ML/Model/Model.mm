#include "MultiArrayNative.h"
#include "Model.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Utils/StdPath.h>

#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sys/sysctl.h>
#include <unistd.h>

namespace eacp::ML
{
bool isSupported()
{
    if (@available(macOS 13.0, iOS 16.0, *))
        return true;

    return false;
}

bool hasComputePlan()
{
    if (@available(macOS 14.4, iOS 17.4, *))
        return true;

    return false;
}

bool hasNeuralEngine()
{
#if TARGET_OS_SIMULATOR
    return false;
#else
    if (@available(macOS 14.0, iOS 17.0, *))
    {
        auto pool = ObjC::AutoReleasePool {};

        for (id<MLComputeDeviceProtocol> device in MLAllComputeDevices())
            if ([(NSObject*) device isKindOfClass:MLNeuralEngineComputeDevice.class])
                return true;
    }

    return false;
#endif
}

FilePath defaultCacheDirectory()
{
    return FilePath::appCacheDirectory() / "CoreML";
}

std::string toString(ComputePlan::Device device)
{
    switch (device)
    {
        case ComputePlan::Device::cpu:
            return "CPU";
        case ComputePlan::Device::gpu:
            return "GPU";
        case ComputePlan::Device::neuralEngine:
            return "NeuralEngine";
        case ComputePlan::Device::unknown:
            break;
    }

    return "unknown";
}

namespace
{
constexpr auto cacheFormat = "eacp-ml-cache-2";
constexpr auto keyLength = 32;
constexpr auto modelPath = "Data/com.apple.CoreML/model.mlmodel";
constexpr auto weightsPath = "Data/com.apple.CoreML/weights/weight.bin";
constexpr auto manifestPath = "Manifest.json";

std::string osBuild()
{
    char build[256] = {};
    auto size = sizeof(build);

    if (sysctlbyname("kern.osversion", build, &size, nullptr, 0) != 0)
        return "unknown";

    return build;
}

Span<const std::uint8_t> bytesOf(const std::string& text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

class CacheKey
{
public:
    explicit CacheKey(const Options& optionsToUse)
        : options(optionsToUse)
    {
        CC_SHA256_Init(&context);
        add(bytesOf(cacheFormat));
    }

    void addFile(const std::string& path, Span<const std::uint8_t> bytes)
    {
        add(bytesOf(path));

        if (path != weightsPath || !hasNamedWeights())
            add(bytes);
    }

    std::string finish()
    {
        if (hasNamedWeights())
        {
            add(bytesOf("named"));
            add(bytesOf(options.weightsName));
            add(bytesOf(options.weightsVersion));
        }

        add(bytesOf(osBuild()));

        unsigned char digest[CC_SHA256_DIGEST_LENGTH] = {};
        CC_SHA256_Final(digest, &context);

        constexpr auto digits = "0123456789abcdef";
        auto text = std::string {};

        for (auto index = 0; index < keyLength / 2; ++index)
        {
            text += digits[digest[index] >> 4];
            text += digits[digest[index] & 0xf];
        }

        return text;
    }

private:
    bool hasNamedWeights() const { return !options.weightsName.empty(); }

    void add(Span<const std::uint8_t> bytes)
    {
        auto length = (std::uint64_t) bytes.getSize();
        addRaw(&length, sizeof(length));
        addRaw(bytes.data(), bytes.getSize());
    }

    void addRaw(const void* data, size_t size)
    {
        constexpr auto chunk = size_t {1} << 30;
        auto bytes = static_cast<const unsigned char*>(data);

        while (size > 0)
        {
            auto count = std::min(size, chunk);
            CC_SHA256_Update(&context, bytes, (CC_LONG) count);
            bytes += count;
            size -= count;
        }
    }

    const Options& options;
    CC_SHA256_CTX context {};
};

std::string keyOf(const Package& package, const Options& options)
{
    auto key = CacheKey {options};
    key.addFile(modelPath, package.model);

    if (!package.weights.empty())
        key.addFile(weightsPath, package.weights);

    auto manifest =
        package.manifest.empty() ? Package::standardManifest() : package.manifest;
    key.addFile(manifestPath, bytesOf(manifest));
    return key.finish();
}

Vector<std::string> filesUnder(const FilePath& directory)
{
    auto root = toStdPath(directory);
    auto files = Vector<std::string> {};
    auto error = std::error_code {};

    for (const auto& entry: std::filesystem::recursive_directory_iterator(root, error))
    {
        auto isHidden = entry.path().filename().string().starts_with(".");

        if (entry.is_regular_file(error) && !isHidden)
            files.add(entry.path().lexically_relative(root).generic_string());
    }

    files.sort();
    return files;
}

std::string keyOf(const FilePath& packageDirectory, const Options& options)
{
    auto key = CacheKey {options};

    for (const auto& path: filesUnder(packageDirectory))
    {
        auto mapped = MemoryMappedFile {packageDirectory / path};

        if (!mapped.isValid())
            return {};

        key.addFile(path, mapped.bytes());
    }

    return key.finish();
}

FilePath cacheDirectoryFor(const Options& options)
{
    return options.cacheDirectory.empty() ? defaultCacheDirectory()
                                          : options.cacheDirectory;
}

std::string uniqueSuffix()
{
    static auto counter = std::atomic<int> {0};
    return std::to_string(getpid()) + "-" + std::to_string(counter++);
}

bool exists(const FilePath& path)
{
    return File {path}.exists();
}

NSURL* urlOf(const FilePath& path)
{
    return [NSURL fileURLWithPath:Strings::toNSString(path.str())];
}

bool startsWithCacheKey(const std::string& name)
{
    auto isHexDigit = [](char c)
    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };

    if ((int) name.size() <= keyLength || name[keyLength] != '.')
        return false;

    for (auto index = 0; index < keyLength; ++index)
        if (!isHexDigit(name[index]))
            return false;

    return true;
}

bool isStaleTemporary(const std::filesystem::directory_entry& entry)
{
    auto name = entry.path().filename().string();
    auto isTemporary =
        name.ends_with(".mlpackage") || name.ends_with(".tmp") || name.ends_with(".trash");

    if (!isTemporary || !startsWithCacheKey(name))
        return false;

    auto error = std::error_code {};
    auto modified = entry.last_write_time(error);

    if (error)
        return false;

    auto age = std::filesystem::file_time_type::clock::now() - modified;
    return age > std::chrono::hours {1};
}

void sweepStaleTemporaries(const FilePath& directory)
{
    auto stale = Vector<FilePath> {};
    auto error = std::error_code {};

    for (const auto& entry:
         std::filesystem::directory_iterator(toStdPath(directory), error))
        if (isStaleTemporary(entry))
            stale.add(FilePath {entry.path()});

    for (const auto& path: stale)
        Files::removeAll(path);
}

void discardDamaged(const FilePath& target)
{
    auto trash = FilePath {target.str() + "." + uniqueSuffix() + ".trash"};

    if (renamex_np(target.c_str(), trash.c_str(), RENAME_EXCL) == 0)
        Files::removeAll(trash);
}

struct Compiled
{
    Result result;
    FilePath path;
    bool hit = false;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
NSURL* compileModel(const FilePath& package, std::string& error)
{
    NSError* compileError = nil;
    auto compiled = [MLModel compileModelAtURL:urlOf(package) error:&compileError];

    if (compiled == nil)
        error = Strings::toStdString(compileError);

    return compiled;
}
#pragma clang diagnostic pop

// Core ML compiles into a temporary directory of its own, possibly on another
// volume, so the result is moved beside the target first and only then
// renamed onto it, the one step that must be atomic. RENAME_EXCL makes a
// second installer fail rather than replace the winner's copy.
Compiled install(NSURL* compiled, const FilePath& target)
{
    auto staged = FilePath {target.str() + "." + uniqueSuffix() + ".tmp"};

    NSError* moveError = nil;
    auto moved = [NSFileManager.defaultManager moveItemAtURL:compiled
                                                       toURL:urlOf(staged)
                                                       error:&moveError];
    if (!moved)
    {
        [NSFileManager.defaultManager removeItemAtURL:compiled error:nil];
        return {Result::failure("Could not move the compiled model: "
                                + Strings::toStdString(moveError)),
                {},
                false};
    }

    if (renamex_np(staged.c_str(), target.c_str(), RENAME_EXCL) == 0)
        return {Result::success(), target, false};

    auto renameError = errno;
    Files::removeAll(staged);

    if (renameError == EEXIST || renameError == ENOTEMPTY)
        return {Result::success(), target, false};

    return {Result::failure("Could not install the compiled model at "
                            + target.str()),
            {},
            false};
}

Compiled compileInto(const FilePath& package, const FilePath& target)
{
    auto error = std::string {};
    auto compiled = compileModel(package, error);

    if (compiled == nil)
        return {Result::failure("Core ML could not compile the model: " + error),
                {},
                false};

    return install(compiled, target);
}

struct Source
{
    const Package* package = nullptr;
    FilePath directory;
};

bool isCompiledModel(const FilePath& directory)
{
    return directory.extension() == ".mlmodelc";
}

struct CacheEntry
{
    Result result;
    FilePath directory;
    std::string key;

    FilePath target() const { return directory / (key + ".mlmodelc"); }
};

CacheEntry locate(const Source& source, const Options& options)
{
    auto entry = CacheEntry {};
    entry.directory = cacheDirectoryFor(options);

    if (source.package != nullptr)
    {
        entry.key = keyOf(*source.package, options);
        entry.result = Result::success();
        return entry;
    }

    if (!exists(source.directory / modelPath))
    {
        entry.result = Result::failure(source.directory.str()
                                       + " is not an ML package: it has no "
                                       + modelPath);
        return entry;
    }

    entry.key = keyOf(source.directory, options);
    entry.result = entry.key.empty()
                       ? Result::failure("Could not read " + source.directory.str())
                       : Result::success();
    return entry;
}

Compiled compileMiss(const Source& source, const CacheEntry& entry)
{
    if (!Files::createDirectories(entry.directory))
        return {Result::failure("Could not create " + entry.directory.str()),
                {},
                false};

    sweepStaleTemporaries(entry.directory);

    if (source.package == nullptr)
        return compileInto(source.directory, entry.target());

    auto packageDirectory =
        entry.directory / (entry.key + "." + uniqueSuffix() + ".mlpackage");

    if (!source.package->write(packageDirectory))
    {
        Files::removeAll(packageDirectory);
        return {Result::failure("Could not write the package at "
                                + packageDirectory.str()),
                {},
                false};
    }

    auto compiled = compileInto(packageDirectory, entry.target());
    Files::removeAll(packageDirectory);
    return compiled;
}

Compiled compileThroughCache(const Source& source, const CacheEntry& entry)
{
    if (exists(entry.target()))
        return {Result::success(), entry.target(), true};

    return compileMiss(source, entry);
}

MLComputeUnits toMLComputeUnits(ComputeUnits units)
{
    switch (units)
    {
        case ComputeUnits::cpu:
            return MLComputeUnitsCPUOnly;
        case ComputeUnits::cpuAndGPU:
            return MLComputeUnitsCPUAndGPU;
        case ComputeUnits::cpuAndNeuralEngine:
            if (@available(macOS 13.0, iOS 16.0, *))
                return MLComputeUnitsCPUAndNeuralEngine;
            return MLComputeUnitsCPUOnly;
        case ComputeUnits::all:
            break;
    }

    return MLComputeUnitsAll;
}

ObjC::Ptr<MLModelConfiguration> makeConfiguration(ComputeUnits units)
{
    auto configuration = ObjC::Ptr<MLModelConfiguration> {[MLModelConfiguration new]};
    configuration.get().computeUnits = toMLComputeUnits(units);
    return configuration;
}

FeatureInfo describe(MLFeatureDescription* description)
{
    auto info = FeatureInfo {};
    info.name = Strings::toStdString(description.name);

    auto constraint = description.multiArrayConstraint;

    if (constraint == nil)
        return info;

    info.shape = toShape(constraint.shape);
    toDType(constraint.dataType, info.type);

    auto shapes = constraint.shapeConstraint;

    if (shapes.type != MLMultiArrayShapeConstraintTypeEnumerated
        || shapes.enumeratedShapes.count < 2)
        return info;

    for (NSArray<NSNumber*>* shape in shapes.enumeratedShapes)
        info.enumeratedShapes.add(toShape(shape));

    return info;
}

Vector<FeatureInfo> describeAll(NSDictionary<NSString*, MLFeatureDescription*>* byName)
{
    auto names = [byName.allKeys sortedArrayUsingSelector:@selector(compare:)];
    auto features = Vector<FeatureInfo> {};

    for (NSString* name in names)
        features.add(describe(byName[name]));

    return features;
}

struct Loaded
{
    ObjC::Ptr<MLModel> model;
    FilePath compiled;
    bool cacheHit = false;
    ComputeUnits units = ComputeUnits::all;
    Vector<FeatureInfo> inputs;
    Vector<FeatureInfo> outputs;
};

struct LoadOutcome
{
    Result result;
    std::shared_ptr<Loaded> loaded;
};

LoadOutcome loadCompiled(const FilePath& compiled, bool cacheHit, ComputeUnits units)
{
    auto pool = ObjC::AutoReleasePool {};
    auto configuration = makeConfiguration(units);

    NSError* error = nil;
    auto model = [MLModel modelWithContentsOfURL:urlOf(compiled)
                                   configuration:configuration.get()
                                           error:&error];
    if (model == nil)
        return {Result::failure("Core ML could not load " + compiled.str() + ": "
                                + Strings::toStdString(error)),
                {}};

    auto loaded = std::make_shared<Loaded>();
    loaded->model.reset(model);
    loaded->compiled = compiled;
    loaded->cacheHit = cacheHit;
    loaded->units = units;
    loaded->inputs = describeAll(model.modelDescription.inputDescriptionsByName);
    loaded->outputs = describeAll(model.modelDescription.outputDescriptionsByName);
    return {Result::success(), loaded};
}

// Core ML reports a damaged compiled model with the same generic error as any
// other load failure, so a hit is only taken for damaged, unpublished and
// recompiled once a second load of it has failed as well.
LoadOutcome loadHit(const Source& source,
                    const CacheEntry& entry,
                    ComputeUnits units)
{
    auto outcome = loadCompiled(entry.target(), true, units);

    if (!outcome.result)
        outcome = loadCompiled(entry.target(), true, units);

    if (outcome.result)
        return outcome;

    discardDamaged(entry.target());

    auto rebuilt = compileThroughCache(source, entry);

    if (!rebuilt.result)
        return {rebuilt.result, {}};

    return loadCompiled(rebuilt.path, rebuilt.hit, units);
}

LoadOutcome loadFromSource(const Source& source, const Options& options)
{
    auto pool = ObjC::AutoReleasePool {};

    if (!isSupported())
        return {Result::failure("Core ML model loading needs macOS 13 or iOS 16"),
                {}};

    if (source.package == nullptr && isCompiledModel(source.directory))
        return loadCompiled(source.directory, false, options.units);

    auto entry = locate(source, options);

    if (!entry.result)
        return {entry.result, {}};

    auto compiled = compileThroughCache(source, entry);

    if (!compiled.result)
        return {compiled.result, {}};

    if (compiled.hit)
        return loadHit(source, entry, options.units);

    return loadCompiled(compiled.path, false, options.units);
}

ComputePlan::Device toDevice(id<MLComputeDeviceProtocol> device)
    API_AVAILABLE(macos(14.0), ios(17.0))
{
    auto object = (NSObject*) device;

    if ([object isKindOfClass:MLNeuralEngineComputeDevice.class])
        return ComputePlan::Device::neuralEngine;

    if ([object isKindOfClass:MLGPUComputeDevice.class])
        return ComputePlan::Device::gpu;

    if ([object isKindOfClass:MLCPUComputeDevice.class])
        return ComputePlan::Device::cpu;

    return ComputePlan::Device::unknown;
}

// The plan names an op by the opset that introduced its current form,
// "ios17.linear"; the graph named it "linear".
std::string withoutOpsetPrefix(const std::string& name)
{
    auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

void readBlock(MLComputePlan* plan,
               MLModelStructureProgramBlock* block,
               ComputePlan& result) API_AVAILABLE(macos(14.4), ios(17.4))
{
    for (MLModelStructureProgramOperation* operation in block.operations)
    {
        for (MLModelStructureProgramBlock* inner in operation.blocks)
            readBlock(plan, inner, result);

        auto type = withoutOpsetPrefix(Strings::toStdString(operation.operatorName));

        if (type == "const")
            continue;

        auto op = ComputePlan::Op {};
        op.type = type;

        if (auto output = operation.outputs.firstObject)
            op.name = Strings::toStdString(output.name);

        if (auto usage = [plan computeDeviceUsageForMLProgramOperation:operation])
        {
            op.device = toDevice(usage.preferredComputeDevice);

            for (id<MLComputeDeviceProtocol> device in usage.supportedComputeDevices)
                op.supported.add(toDevice(device));
        }

        if (auto cost = [plan estimatedCostOfMLProgramOperation:operation])
            op.cost = cost.weight;

        result.ops.add(op);
    }
}

ComputePlan readPlan(const Loaded& loaded) API_AVAILABLE(macos(14.4), ios(17.4))
{
    auto pool = ObjC::AutoReleasePool {};
    auto configuration = makeConfiguration(loaded.units);
    auto done = dispatch_semaphore_create(0);

    __block MLComputePlan* plan = nil;
    __block NSError* failure = nil;

    auto keepPlan = ^(MLComputePlan* result, NSError* error)
    {
        plan = [result retain];
        failure = [error retain];
        dispatch_semaphore_signal(done);
    };

    [MLComputePlan loadContentsOfURL:urlOf(loaded.compiled)
                       configuration:configuration.get()
                   completionHandler:keepPlan];

    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);

    auto result = ComputePlan {};

    if (plan == nil)
    {
        LOG("Core ML could not build the compute plan of ",
            loaded.compiled.str(),
            ": ",
            Strings::toStdString(failure));
        [failure release];
        return result;
    }

    [failure release];

    auto program = plan.modelStructure.program;
    auto function = program.functions[@"main"];

    if (function == nil)
        function = program.functions.allValues.firstObject;

    if (function != nil)
        readBlock(plan, function.block, result);

    [plan release];
    return result;
}

Prediction runPrediction(Loaded& loaded, const Inputs& inputs, const Outputs& bound)
{
    auto pool = ObjC::AutoReleasePool {};
    auto features = [NSMutableDictionary dictionary];

    for (const auto& input: loaded.inputs)
    {
        auto array = inputs.getValue(input.name);

        if (array == nullptr || !array->isValid())
            return {Result::failure("No array given for the input " + input.name),
                    {}};

        features[Strings::toNSString(input.name)] =
            [MLFeatureValue featureValueWithMultiArray:nativeArray(*array)];
    }

    NSError* error = nil;
    auto provider = ObjC::Ptr<MLDictionaryFeatureProvider> {
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:features
                                                          error:&error]};
    if (!provider)
        return {Result::failure(Strings::toStdString(error)), {}};

    auto options = ObjC::Ptr<MLPredictionOptions> {[MLPredictionOptions new]};
    auto backings = [NSMutableDictionary dictionary];

    for (const auto& output: loaded.outputs)
        if (auto array = bound.getValue(output.name); array && array->isValid())
            backings[Strings::toNSString(output.name)] = nativeArray(*array);

    if (@available(macOS 11.0, iOS 16.0, *))
        if (backings.count > 0)
            options.get().outputBackings = backings;

    auto result = [loaded.model.get() predictionFromFeatures:provider.get()
                                                     options:options.get()
                                                       error:&error];
    if (result == nil)
        return {Result::failure("Core ML prediction failed: "
                                + Strings::toStdString(error)),
                {}};

    auto prediction = Prediction {Result::success(), {}};

    for (const auto& output: loaded.outputs)
    {
        auto value =
            [result featureValueForName:Strings::toNSString(output.name)].multiArrayValue;
        auto array = bound.getValue(output.name);

        if (array == nullptr || !array->isValid())
        {
            prediction.outputs[output.name] = adoptOutput(value);
            continue;
        }

        auto boundArray = *array;

        if (value != nil && value != nativeArray(boundArray))
            boundArray.copyFrom(adoptOutput(value));

        prediction.outputs[output.name] = boundArray;
    }

    return prediction;
}

struct Shared
{
    Shared()
        : queue(dispatch_queue_create("eacp.ml.model", DISPATCH_QUEUE_SERIAL))
    {
    }

    ~Shared() { dispatch_release(queue); }

    std::shared_ptr<Loaded> current() const
    {
        auto guard = std::lock_guard {lock};
        return loaded;
    }

    Result install(const LoadOutcome& outcome)
    {
        auto guard = std::lock_guard {lock};
        loaded = outcome.loaded;
        return outcome.result;
    }

    Result load(const Source& source, const Options& options)
    {
        return install(loadFromSource(source, options));
    }

    Prediction predict(const Inputs& inputs, const Outputs& bound)
    {
        auto model = current();

        if (model == nullptr)
            return {Result::failure("No model is loaded"), {}};

        auto guard = std::lock_guard {predicting};
        return runPrediction(*model, inputs, bound);
    }

    mutable std::mutex lock;
    std::mutex predicting;
    std::shared_ptr<Loaded> loaded;
    dispatch_queue_t queue;
};

class PendingJobs
{
public:
    int add(const Callback& abandon)
    {
        auto guard = std::lock_guard {lock};
        auto id = nextJob++;
        abandoners[id] = abandon;
        return id;
    }

    template <typename Resolve>
    void settle(int id, const Resolve& resolve)
    {
        auto guard = std::lock_guard {lock};

        if (!alive)
            return;

        abandoners.remove(id);
        resolve();
    }

    void abandonAll()
    {
        auto pending = EA::MapVector<int, Callback> {};

        {
            auto guard = std::lock_guard {lock};
            alive = false;
            pending = abandoners;
            abandoners.clear();
        }

        auto abandonPending = [pending]
        {
            for (const auto& job: pending)
                job.second();
        };

        if (Threads::isMainThread())
            abandonPending();
        else
            Threads::callAsync(abandonPending);
    }

private:
    std::recursive_mutex lock;
    bool alive = true;
    EA::MapVector<int, Callback> abandoners;
    int nextJob = 0;
};
} // namespace

struct Model::Native
{
    ~Native() { jobs->abandonAll(); }

    template <typename T, typename Work>
    Threads::Async<T> runOnQueueResolvingOnMain(const Work& work)
    {
        Threads::assertMainThread();

        auto promise = Threads::AsyncPromise<T> {};
        auto abandonJob = [promise] { promise.abandon(); };
        auto id = jobs->add(abandonJob);
        auto pending = jobs;
        auto job = work;

        auto runJob = ^{
          auto value = job();

          auto resolveJob = [pending, id, promise, value]
          {
              auto resolvePromise = [&promise, &value] { promise.resolve(value); };
              pending->settle(id, resolvePromise);
          };

          Threads::callAsync(resolveJob);
        };

        dispatch_async(shared->queue, runJob);
        return promise.get();
    }

    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::shared_ptr<PendingJobs> jobs = std::make_shared<PendingJobs>();
};

Model::Model() = default;
Model::~Model() = default;

Result Model::load(const Package& package, const Options& options)
{
    auto source = Source {};
    source.package = &package;
    return impl->shared->load(source, options);
}

Result Model::load(const FilePath& modelDirectory, const Options& options)
{
    auto source = Source {};
    source.directory = modelDirectory;
    return impl->shared->load(source, options);
}

Threads::Async<Result> Model::loadAsync(const Package& package, const Options& options)
{
    auto shared = impl->shared;
    auto owned = std::make_shared<Package>(package);

    auto loadPackage = [shared, owned, options]
    {
        auto source = Source {};
        source.package = owned.get();
        return shared->load(source, options);
    };

    return impl->runOnQueueResolvingOnMain<Result>(loadPackage);
}

Threads::Async<Result> Model::loadAsync(const FilePath& modelDirectory,
                                        const Options& options)
{
    auto shared = impl->shared;

    auto loadDirectory = [shared, modelDirectory, options]
    {
        auto source = Source {};
        source.directory = modelDirectory;
        return shared->load(source, options);
    };

    return impl->runOnQueueResolvingOnMain<Result>(loadDirectory);
}

bool Model::isLoaded() const
{
    return impl->shared->current() != nullptr;
}

bool Model::wasCacheHit() const
{
    auto loaded = impl->shared->current();
    return loaded != nullptr && loaded->cacheHit;
}

FilePath Model::compiledPath() const
{
    auto loaded = impl->shared->current();
    return loaded ? loaded->compiled : FilePath {};
}

ComputeUnits Model::units() const
{
    auto loaded = impl->shared->current();
    return loaded ? loaded->units : ComputeUnits::all;
}

Vector<FeatureInfo> Model::inputs() const
{
    auto loaded = impl->shared->current();
    return loaded ? loaded->inputs : Vector<FeatureInfo> {};
}

Vector<FeatureInfo> Model::outputs() const
{
    auto loaded = impl->shared->current();
    return loaded ? loaded->outputs : Vector<FeatureInfo> {};
}

Result Model::predict(const Inputs& inputs, Outputs& outputs)
{
    auto prediction = impl->shared->predict(inputs, outputs);

    for (auto& output: prediction.outputs)
        if (outputs.getValue(output.first) == nullptr)
            outputs[output.first] = output.second;

    return prediction;
}

Threads::Async<Prediction> Model::predictAsync(const Inputs& inputs,
                                               const Outputs& outputs)
{
    auto shared = impl->shared;

    auto predictOnQueue = [shared, inputs, outputs]
    { return shared->predict(inputs, outputs); };

    return impl->runOnQueueResolvingOnMain<Prediction>(predictOnQueue);
}

ComputePlan Model::computePlan() const
{
    if (@available(macOS 14.4, iOS 17.4, *))
    {
        if (auto loaded = impl->shared->current())
            return readPlan(*loaded);
    }

    return {};
}
} // namespace eacp::ML
