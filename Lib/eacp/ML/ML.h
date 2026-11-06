/** @file
    Machine learning on Core ML. Two halves: ML::Graph builds a tensor program
    (inputs, fp16 weights, transformer ops, elementwise bodies written over
    GPU::Float) into a Package, on macOS, Windows and Linux; ML::Model loads a
    Package, .mlpackage or .mlmodelc and runs predictions, on Apple only
    (EACP_HAS_COREML).

    - Errors are values: a bad op returns an invalid Tensor and build() is then
      empty; load and predict return a Result. Nothing throws.
    - predict() blocks on the caller's thread; predictAsync() runs on the
      model's serial queue and resolves on the main thread.
    - Compiled models are cached and reused by hash; see Model.
    - Lib/eacp/ML/README.md is the long account.
*/
#pragma once

#include "Graph/Graph.h"
#include "MIL/Blob.h"
#include "MIL/Half.h"
#include "MIL/MILWriter.h"
#include "MIL/Package.h"
#include "MIL/Protobuf.h"

#if EACP_HAS_COREML
#include "Model/MultiArray.h"
#include "Model/ComputePlan.h"
#include "Model/Model.h"
#include "Model/Options.h"
#endif
