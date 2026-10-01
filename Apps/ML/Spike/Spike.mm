#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>
#include <Accelerate/Accelerate.h>

#include "MILWriter.h"
#include <eacp/Core/Utils/FilePath.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// Phase 0 runtime spike: write a [1500, 384] x [384, 384] linear (or matmul) +
// softmax package through MILWriter, compile it at run time, load it per compute
// unit setting, read the compute plan, predict from plain and IOSurface-backed
// fp16 arrays, and compare against an fp32 reference. Given a package path it
// measures that package instead.
//
//   MLSpike [--op linear|matmul] [--variant blob|inline|enum]... [--rows N]...
//           [--runs N] [--plan-only] [--keep-compiled]
//   MLSpike <model.mlpackage | model.mlmodelc> --weights raw_f16.bin [...]
namespace
{
using Clock = std::chrono::steady_clock;
using Half = _Float16;

constexpr auto columns = 384;

struct Options
{
    std::string package;
    std::string weights;
    std::string op = "linear";
    std::vector<std::string> variants;
    std::vector<int> rows;
    int runs = 20;
    bool planOnly = false;
    bool keepCompiled = false;
};

struct Setting
{
    const char* name;
    MLComputeUnits units;
};

const Setting settings[] = {
    {"CPUAndNeuralEngine", MLComputeUnitsCPUAndNeuralEngine},
    {"All", MLComputeUnitsAll},
    {"CPUAndGPU", MLComputeUnitsCPUAndGPU},
    {"CPUOnly", MLComputeUnitsCPUOnly},
};

struct Stats
{
    double median = 0.0;
    double min = 0.0;
};

struct Errors
{
    double maxAbs = 0.0;
    double maxRel = 0.0;
};

double msSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[noreturn]] void fail(const char* what, NSError* error)
{
    std::fprintf(stderr,
                 "%s failed: %s\n",
                 what,
                 error ? error.localizedDescription.UTF8String : "(no error)");
    std::exit(1);
}

[[noreturn]] void usage()
{
    std::fprintf(stderr,
                 "usage: MLSpike [--op linear|matmul] [--variant blob|inline|enum]"
                 "... [--rows N]... [--runs N] [--plan-only] [--keep-compiled]\n"
                 "       MLSpike <model.mlpackage | model.mlmodelc> --weights "
                 "raw_f16.bin [--rows N]... [--runs N] [--plan-only]\n");
    std::exit(2);
}

Options parseOptions(int argc, const char* argv[])
{
    auto options = Options {};

    for (auto i = 1; i < argc; ++i)
    {
        auto arg = std::string {argv[i]};
        auto hasValue = i + 1 < argc;

        if (arg == "--rows" && hasValue)
            options.rows.push_back(std::atoi(argv[++i]));
        else if (arg == "--runs" && hasValue)
            options.runs = std::atoi(argv[++i]);
        else if (arg == "--keep-compiled")
            options.keepCompiled = true;
        else if (arg == "--plan-only")
            options.planOnly = true;
        else if (arg == "--weights" && hasValue)
            options.weights = argv[++i];
        else if (arg == "--op" && hasValue)
            options.op = argv[++i];
        else if (arg == "--variant" && hasValue)
            options.variants.push_back(argv[++i]);
        else if (arg.starts_with("--"))
            usage();
        else
            options.package = arg;
    }

    if (options.op != "linear" && options.op != "matmul")
        usage();

    if (options.variants.empty())
        options.variants = {"blob", "inline", "enum"};

    return options;
}

std::vector<char> readFile(const std::string& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

// A single-weight package keeps its fp16 tensor at the end of weight.bin.
std::vector<float> loadWeights(const Options& options)
{
    auto path = options.weights.empty()
                    ? options.package + "/Data/com.apple.CoreML/weights/weight.bin"
                    : options.weights;
    auto bytes = readFile(path);
    auto count = columns * columns;
    auto byteCount = count * (int) sizeof(Half);

    if ((int) bytes.size() < byteCount)
    {
        std::fprintf(stderr, "weights file %s too small\n", path.c_str());
        std::exit(1);
    }

    auto halves = reinterpret_cast<const Half*>(bytes.data() + bytes.size()
                                                 - (size_t) byteCount);
    return {halves, halves + count};
}

std::vector<Half> generateWeights()
{
    auto engine = std::mt19937 {1234u};
    auto normal = std::normal_distribution<float> {0.0f, 0.05f};
    auto values = std::vector<Half>((size_t) (columns * columns));

    for (auto& value: values)
        value = (Half) normal(engine);

    return values;
}

std::vector<Half> transposed(const std::vector<Half>& weights)
{
    auto result = std::vector<Half>(weights.size());

    for (auto in = 0; in < columns; ++in)
        for (auto out = 0; out < columns; ++out)
            result[(size_t) (out * columns + in)] =
                weights[(size_t) (in * columns + out)];

    return result;
}

struct Variant
{
    std::string name;
    MILWriter::ConstStorage storage;
    std::vector<MILWriter::Specification::Shape> shapes;
    std::vector<int> rows;
};

Variant makeVariant(const std::string& name)
{
    using MILWriter::ConstStorage;

    if (name == "inline")
        return {name, ConstStorage::Inline, {{1500, columns}}, {1500}};

    if (name == "enum")
        return {name,
                ConstStorage::Blob,
                {{1500, columns}, {448, columns}, {512, columns}, {1024, columns}},
                {1500, 448}};

    if (name != "blob")
        usage();

    return {name, ConstStorage::Blob, {{1500, columns}}, {1500}};
}

MILWriter::Graph buildGraph(const std::string& op,
                            const Variant& variant,
                            const std::vector<Half>& weights)
{
    using namespace MILWriter;

    auto graph = Graph {};
    auto x = graph.input("x", MIL::DataType::Float16, variant.shapes);
    auto square = MIL::TensorType {MIL::DataType::Float16, {{columns}, {columns}}};

    if (op == "matmul")
    {
        auto y = graph.constant(
            "mm_y_0", square, asBytes(std::span<const Half> {weights}), variant.storage);
        graph.output(graph.softmax("y", graph.matmul("mm", x, y), -1));
        return graph;
    }

    auto weightOutIn = transposed(weights);
    auto weight = graph.constant("mm_weight_0",
                                 square,
                                 asBytes(std::span<const Half> {weightOutIn}),
                                 variant.storage);
    auto zeros = std::vector<Half>((size_t) columns, (Half) 0);
    auto bias = graph.constant("mm_bias_0",
                               {MIL::DataType::Float16, {{columns}}},
                               asBytes(std::span<const Half> {zeros}),
                               variant.storage);
    graph.output(graph.softmax("y", graph.linear("mm", x, weight, bias), -1));
    return graph;
}

uintmax_t fileSize(const std::filesystem::path& path)
{
    auto error = std::error_code {};
    auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

void writePackageTimed(const std::filesystem::path& path,
                       const std::string& op,
                       const Variant& variant,
                       const std::vector<Half>& weights,
                       int attempt)
{
    auto start = Clock::now();
    MILWriter::Package::write(path, buildGraph(op, variant, weights));
    auto elapsed = msSince(start);

    auto data = path / "Data" / "com.apple.CoreML";
    std::printf("write #%d: %.2f ms (model.mlmodel %ju bytes, weight.bin %ju "
                "bytes)\n",
                attempt,
                elapsed,
                fileSize(data / "model.mlmodel"),
                fileSize(data / "weights" / "weight.bin"));
}

std::vector<float> randomInput(int rows, unsigned seed)
{
    auto engine = std::mt19937 {seed};
    auto normal = std::normal_distribution<float> {0.0f, 1.0f};
    auto values = std::vector<float>((size_t) (rows * columns));

    for (auto& value: values)
        value = (float) (Half) normal(engine);

    return values;
}

std::vector<float> reference(const std::vector<float>& x,
                             const std::vector<float>& weights,
                             int rows)
{
    auto y = std::vector<float>(x.size());
    cblas_sgemm(CblasRowMajor,
                CblasNoTrans,
                CblasNoTrans,
                rows,
                columns,
                columns,
                1.0f,
                x.data(),
                columns,
                weights.data(),
                columns,
                0.0f,
                y.data(),
                columns);

    for (auto r = 0; r < rows; ++r)
    {
        auto row = y.data() + r * columns;
        auto top = *std::max_element(row, row + columns);
        auto sum = 0.0;

        for (auto c = 0; c < columns; ++c)
        {
            row[c] = std::exp(row[c] - top);
            sum += row[c];
        }

        for (auto c = 0; c < columns; ++c)
            row[c] = (float) (row[c] / sum);
    }

    return y;
}

Errors compare(const std::vector<float>& actual, const std::vector<float>& expected)
{
    auto errors = Errors {};

    for (size_t i = 0; i < actual.size(); ++i)
    {
        auto diff = std::abs((double) actual[i] - expected[i]);
        errors.maxAbs = std::max(errors.maxAbs, diff);

        if (expected[i] >= 1e-4f)
            errors.maxRel = std::max(errors.maxRel, diff / expected[i]);
    }

    return errors;
}

MLModelConfiguration* makeConfiguration(MLComputeUnits units)
{
    auto configuration = [MLModelConfiguration new];
    configuration.computeUnits = units;
    return configuration;
}

MLMultiArray* plainArray(int rows)
{
    NSError* error = nil;
    auto array = [[MLMultiArray alloc] initWithShape:@[@(rows), @(columns)]
                                            dataType:MLMultiArrayDataTypeFloat16
                                               error:&error];
    if (!array)
        fail("MLMultiArray initWithShape", error);

    return array;
}

MLMultiArray* surfaceArray(int rows)
{
    NSDictionary* attributes = @{
        (id) kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id) kCVPixelBufferMetalCompatibilityKey: @YES,
    };

    CVPixelBufferRef buffer = nullptr;
    auto status = CVPixelBufferCreate(kCFAllocatorDefault,
                                      columns,
                                      (size_t) rows,
                                      kCVPixelFormatType_OneComponent16Half,
                                      (__bridge CFDictionaryRef) attributes,
                                      &buffer);
    if (status != kCVReturnSuccess)
    {
        std::fprintf(stderr, "CVPixelBufferCreate failed: %d\n", status);
        std::exit(1);
    }

    auto array = [[MLMultiArray alloc] initWithPixelBuffer:buffer
                                                     shape:@[@(rows), @(columns)]];
    CVPixelBufferRelease(buffer);
    return array;
}

void fill(MLMultiArray* array, const std::vector<float>& values)
{
    [array getMutableBytesWithHandler:^(
               void* bytes, NSInteger, NSArray<NSNumber*>* strides) {
        auto rowStride = strides[0].integerValue;
        auto rows = array.shape[0].intValue;
        auto out = static_cast<Half*>(bytes);

        for (auto r = 0; r < rows; ++r)
            for (auto c = 0; c < columns; ++c)
                out[r * rowStride + c] = (Half) values[(size_t) (r * columns + c)];
    }];
}

std::vector<float> read(MLMultiArray* array)
{
    auto rows = array.shape[0].intValue;
    auto rowStride = array.strides[0].integerValue;
    auto values = std::vector<float>((size_t) (rows * columns));
    auto out = values.data();

    [array getBytesWithHandler:^(const void* bytes, NSInteger) {
        auto in = static_cast<const Half*>(bytes);

        for (auto r = 0; r < rows; ++r)
            for (auto c = 0; c < columns; ++c)
                out[r * columns + c] = (float) in[r * rowStride + c];
    }];

    return values;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
NSURL* compileTimed(NSURL* package, int attempt)
{
    NSError* error = nil;
    auto start = Clock::now();
    auto compiled = [MLModel compileModelAtURL:package error:&error];
    auto elapsed = msSince(start);

    if (!compiled)
        fail("compileModelAtURL", error);

    std::printf("compile #%d: %.1f ms -> %s\n", attempt, elapsed, compiled.path.UTF8String);
    return compiled;
}
#pragma clang diagnostic pop

NSURL* compileToStablePath(NSURL* package, NSURL* directory, bool keepCompiled)
{
    auto first = compileTimed(package, 1);
    auto second = compileTimed(package, 2);
    [NSFileManager.defaultManager removeItemAtURL:second error:nil];

    [NSFileManager.defaultManager createDirectoryAtURL:directory
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];

    auto name = [package.lastPathComponent.stringByDeletingPathExtension
        stringByAppendingPathExtension:@"mlmodelc"];
    auto stable = [directory URLByAppendingPathComponent:name];

    if (keepCompiled && [stable checkResourceIsReachableAndReturnError:nil])
    {
        [NSFileManager.defaultManager removeItemAtURL:first error:nil];
        std::printf("reusing compiled model at %s\n", stable.path.UTF8String);
        return stable;
    }

    [NSFileManager.defaultManager removeItemAtURL:stable error:nil];

    NSError* error = nil;
    if (![NSFileManager.defaultManager moveItemAtURL:first toURL:stable error:&error])
        fail("move compiled model", error);

    std::printf("compiled model at %s\n", stable.path.UTF8String);
    return stable;
}

MLModel* loadTimed(NSURL* compiled, const Setting& setting, int attempt)
{
    NSError* error = nil;
    auto start = Clock::now();
    auto model = [MLModel modelWithContentsOfURL:compiled
                                   configuration:makeConfiguration(setting.units)
                                           error:&error];
    auto elapsed = msSince(start);

    if (!model)
        fail("modelWithContentsOfURL", error);

    std::printf("  load #%d: %.1f ms\n", attempt, elapsed);
    return model;
}

const char* deviceName(id<MLComputeDeviceProtocol> device)
{
    auto object = (NSObject*) device;

    if ([object isKindOfClass:MLNeuralEngineComputeDevice.class])
        return "NeuralEngine";

    if ([object isKindOfClass:MLGPUComputeDevice.class])
        return "GPU";

    if ([object isKindOfClass:MLCPUComputeDevice.class])
        return "CPU";

    return "?";
}

std::string supportedNames(MLComputePlanDeviceUsage* usage)
{
    auto names = std::string {};

    for (id<MLComputeDeviceProtocol> device in usage.supportedComputeDevices)
    {
        if (!names.empty())
            names += ",";

        names += deviceName(device);
    }

    return names;
}

MLComputePlan* loadPlan(NSURL* compiled, MLComputeUnits units)
{
    __block MLComputePlan* plan = nil;
    __block NSError* planError = nil;
    auto done = dispatch_semaphore_create(0);

    [MLComputePlan loadContentsOfURL:compiled
                       configuration:makeConfiguration(units)
                   completionHandler:^(MLComputePlan* result, NSError* error) {
                       plan = result;
                       planError = error;
                       dispatch_semaphore_signal(done);
                   }];

    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

    if (!plan)
        fail("MLComputePlan loadContentsOfURL", planError);

    return plan;
}

void printPlan(NSURL* compiled, const Setting& setting)
{
    auto start = Clock::now();
    auto plan = loadPlan(compiled, setting.units);
    std::printf("  plan (%.1f ms):\n", msSince(start));

    auto function = plan.modelStructure.program.functions[@"main"];

    for (MLModelStructureProgramOperation* operation in function.block.operations)
    {
        auto usage = [plan computeDeviceUsageForMLProgramOperation:operation];
        auto cost = [plan estimatedCostOfMLProgramOperation:operation];
        auto output = operation.outputs.firstObject.name;

        std::printf("    %-8s %-18s preferred %-12s supported %-22s cost %s\n",
                    operation.operatorName.UTF8String,
                    output ? output.UTF8String : "-",
                    usage ? deviceName(usage.preferredComputeDevice) : "-",
                    usage ? supportedNames(usage).c_str() : "-",
                    cost ? [NSString stringWithFormat:@"%.4f", cost.weight].UTF8String
                         : "-");
    }
}

Stats summarize(std::vector<double> times)
{
    std::sort(times.begin(), times.end());
    return {times[times.size() / 2], times.front()};
}

struct Prediction
{
    std::vector<float> values;
    Stats stats;
};

Prediction predict(MLModel* model,
                   MLMultiArray* input,
                   MLMultiArray* outputBacking,
                   int runs)
{
    auto inputName = model.modelDescription.inputDescriptionsByName.allKeys.firstObject;
    auto outputName =
        model.modelDescription.outputDescriptionsByName.allKeys.firstObject;

    NSError* error = nil;
    auto features = [[MLDictionaryFeatureProvider alloc]
        initWithDictionary:@{inputName: [MLFeatureValue featureValueWithMultiArray:input]}
                     error:&error];
    if (!features)
        fail("MLDictionaryFeatureProvider", error);

    auto options = [MLPredictionOptions new];
    auto result = (id<MLFeatureProvider>) nil;

    auto runOnce = [&]
    {
        if (outputBacking)
            options.outputBackings = @{outputName: outputBacking};

        NSError* predictError = nil;
        result = [model predictionFromFeatures:features
                                       options:options
                                         error:&predictError];
        if (!result)
            fail("predictionFromFeatures", predictError);
    };

    for (auto i = 0; i < 3; ++i)
        runOnce();

    auto times = std::vector<double> {};

    for (auto i = 0; i < runs; ++i)
    {
        auto start = Clock::now();
        runOnce();
        times.push_back(msSince(start));
    }

    auto output = [result featureValueForName:outputName].multiArrayValue;

    if (outputBacking && output != outputBacking)
        std::printf("    (output did not come back in the backing array)\n");

    return {read(output), summarize(times)};
}

void runShape(MLModel* model,
              const std::vector<float>& weights,
              int rows,
              int runs)
{
    auto x = randomInput(rows, 42u + (unsigned) rows);
    auto expected = reference(x, weights, rows);

    auto plain = plainArray(rows);
    fill(plain, x);
    auto surface = surfaceArray(rows);
    fill(surface, x);

    struct Form
    {
        const char* name;
        MLMultiArray* input;
        MLMultiArray* output;
    };

    const Form forms[] = {
        {"plain", plain, nil},
        {"surface-in", surface, nil},
        {"surface-in+out", surface, surfaceArray(rows)},
    };

    for (const auto& form: forms)
    {
        auto prediction = predict(model, form.input, form.output, runs);
        auto errors = compare(prediction.values, expected);

        std::printf("    rows %-5d %-15s median %7.3f ms  min %7.3f ms  "
                    "maxAbs %.3g  maxRel %.3g\n",
                    rows,
                    form.name,
                    prediction.stats.median,
                    prediction.stats.min,
                    errors.maxAbs,
                    errors.maxRel);
    }
}

std::vector<int> defaultRows(MLModel* model)
{
    auto input = model.modelDescription.inputDescriptionsByName.allValues.firstObject;
    return {input.multiArrayConstraint.shape[0].intValue};
}

void describeInput(MLModel* model)
{
    auto input = model.modelDescription.inputDescriptionsByName.allValues.firstObject;
    auto constraint = input.multiArrayConstraint;
    std::printf("input %s shape %s", input.name.UTF8String,
                [constraint.shape componentsJoinedByString:@"x"].UTF8String);

    for (NSArray<NSNumber*>* shape in constraint.shapeConstraint.enumeratedShapes)
        std::printf(" | %s", [shape componentsJoinedByString:@"x"].UTF8String);

    std::printf("\n");
}
void measure(NSURL* package,
             NSURL* compiledDirectory,
             const std::vector<float>& weights,
             const std::vector<int>& variantRows,
             const Options& options)
{
    std::printf("package %s\n", package.path.UTF8String);
    auto isCompiled = [package.pathExtension isEqualToString:@"mlmodelc"];
    auto compiled =
        isCompiled ? package
                   : compileToStablePath(
                         package, compiledDirectory, options.keepCompiled);

    for (const auto& setting: settings)
    {
        std::printf("\n[%s]\n", setting.name);
        loadTimed(compiled, setting, 1);
        auto model = loadTimed(compiled, setting, 2);

        if (&setting == &settings[0])
            describeInput(model);

        printPlan(compiled, setting);

        if (options.planOnly)
            continue;

        auto rows = !options.rows.empty() ? options.rows
                    : !variantRows.empty() ? variantRows
                                           : defaultRows(model);

        for (auto count: rows)
            runShape(model, weights, count, options.runs);
    }
}

NSURL* directoryURL(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path);
    return [NSURL fileURLWithPath:@(path.c_str()) isDirectory:YES];
}

void measureGenerated(const Options& options)
{
    auto halves = generateWeights();
    auto weights = std::vector<float>(halves.begin(), halves.end());
    auto root = std::filesystem::path {
        (eacp::FilePath::appCacheDirectory() / "Spike").str()};
    auto compiledDirectory = directoryURL(root / "Compiled");

    for (const auto& name: options.variants)
    {
        auto variant = makeVariant(name);
        auto path = root / (options.op + "_softmax_" + name + ".mlpackage");

        std::printf("\n==== %s %s ====\n", options.op.c_str(), name.c_str());
        writePackageTimed(path, options.op, variant, halves, 1);
        writePackageTimed(path, options.op, variant, halves, 2);

        measure([NSURL fileURLWithPath:@(path.c_str())],
                compiledDirectory,
                weights,
                variant.rows,
                options);
    }
}
} // namespace

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        auto options = parseOptions(argc, argv);

        if (options.package.empty())
        {
            measureGenerated(options);
            return 0;
        }

        auto temporary =
            std::filesystem::path {NSTemporaryDirectory().UTF8String};
        measure([NSURL fileURLWithPath:@(options.package.c_str())],
                directoryURL(temporary / "eacp-ml-spike"),
                loadWeights(options),
                {},
                options);
    }

    return 0;
}
