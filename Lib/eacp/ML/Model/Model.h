#pragma once

#include "MultiArray.h"
#include "ComputePlan.h"
#include "Options.h"

#include "../MIL/Package.h"

#include <ea_data_structures/Structures/MapVector.h>

namespace eacp::ML
{
// macOS 13 / iOS 16: the engine-only compute units and output backings.
bool isSupported();

// Whether this OS loads a model of that specification version, as
// MIL::Specification::specificationVersion gives it: 6 wants macOS 12 / iOS
// 15, 7 macOS 13 / iOS 16, 8 (the CoreML7 opset) macOS 14 / iOS 17, and 9
// (CoreML8, which scaledDotProductAttention forces) macOS 15 / iOS 18. False
// for a version newer than 9.
bool supportsSpecification(int version);

// macOS 14.4 / iOS 17.4: Model::computePlan().
bool hasComputePlan();

// Whether this machine has a Neural Engine Core ML can place on. False
// wherever that cannot be asked (before macOS 14 / iOS 17) and in the
// simulator.
bool hasNeuralEngine();

FilePath defaultCacheDirectory();

// Named arrays, the names exactly those given to Graph::input and output.
using Features = EA::MapVector<std::string, MultiArray>;
using Inputs = Features;
using Outputs = Features;

struct FeatureInfo
{
    std::string name;
    Shape shape;
    DType type = DType::float32;

    // Empty unless the input was declared with a list of shapes; shape is
    // then the default member.
    Vector<Shape> enumeratedShapes;
};

// A prediction's outcome and every output it produced: the arrays the caller
// bound, which it already shares, and one the runner allocated for each
// output it did not.
struct Prediction : Result
{
    Outputs outputs;
};

// A Core ML model compiled from an ML Program package and loaded for one set
// of compute units.
//
// The compile is cached under the cache directory as <hash>.mlmodelc, the
// hash taken over the program's bytes, the weights' identity and the OS
// build. A hit is loaded where it lies and never recompiled or replaced,
// because Core ML keys its engine cache on that directory: the first load onto
// the Neural Engine costs tens of milliseconds, a load of the same compiled
// model afterwards a few. A miss compiles into a temporary directory and
// renames it into place; one that loses that race to another process loads
// the winner's copy and deletes its own. A hit that fails to load twice is
// taken for damaged, moved out of the way and recompiled.
//
// The blocking forms run on the caller's thread and pump no loop, so a worker
// thread or a console app can use them. The async forms run on a serial queue
// of the model's own, in the order they were called, and resolve on the main
// thread; they are called from the main thread. One model runs one prediction
// at a time, whichever form asked for it. The model can be destroyed on any
// thread; that abandons every Async it handed out, and nothing they captured
// is called afterwards.
class Model
{
public:
    Model();
    ~Model();

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    Result load(const Package& package, const Options& options = {});

    // An .mlpackage directory, compiled through the cache like a Package, or
    // an .mlmodelc, loaded where it lies.
    Result load(const FilePath& modelDirectory, const Options& options = {});

    Threads::Async<Result> loadAsync(const Package& package,
                                     const Options& options = {});
    Threads::Async<Result> loadAsync(const FilePath& modelDirectory,
                                     const Options& options = {});

    bool isLoaded() const;

    // Whether the load found its compiled model in the cache. False after a
    // compile and for an .mlmodelc loaded where it lies.
    bool wasCacheHit() const;
    FilePath compiledPath() const;
    ComputeUnits units() const;

    // Sorted by name.
    Vector<FeatureInfo> inputs() const;
    Vector<FeatureInfo> outputs() const;

    // Every model input must be in inputs. An array in outputs under an
    // output's name is passed to Core ML as that output's backing, so the
    // result is written straight into it; each output not bound is allocated
    // and added to outputs.
    Result predict(const Inputs& inputs, Outputs& outputs);

    // predict() on the model's queue. The bound outputs are the caller's own
    // arrays, shared, and so are filled in place; the Prediction carries them
    // and whatever was allocated.
    Threads::Async<Prediction> predictAsync(const Inputs& inputs,
                                            const Outputs& outputs = {});

    // Empty when no model is loaded or the OS has no MLComputePlan. Blocks
    // while Core ML builds the plan, which can take as long as a first load.
    ComputePlan computePlan() const;

private:
    struct Native;

    Pimpl<Native> impl;
};
} // namespace eacp::ML
