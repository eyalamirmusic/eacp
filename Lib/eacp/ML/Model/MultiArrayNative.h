#pragma once

#import <CoreML/CoreML.h>

#include "MultiArray.h"

namespace eacp::ML
{
Shape toShape(NSArray<NSNumber*>* dimensions);

bool toDType(MLMultiArrayDataType dataType, DType& type);

MLMultiArray* nativeArray(const MultiArray& array);

// An array Core ML allocated for an output nobody bound: shared as it is when
// it lies in a pixel buffer, copied into a MultiArray of our own otherwise.
MultiArray adoptOutput(MLMultiArray* output);
} // namespace eacp::ML
