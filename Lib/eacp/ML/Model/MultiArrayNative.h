#pragma once

#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>

#include "MultiArray.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::ML
{
// A surface-backed array keeps no pointer: its base address is only valid
// while the pixel buffer is locked, so every CPU access locks it for its own
// duration. bytes is the plain array's memory, owned by the MLMultiArray.
struct MultiArray::Native
{
    Shape shape;
    DType type = DType::float32;
    ObjC::Ptr<MLMultiArray> array;
    CFRef<CVPixelBufferRef> surface;
    std::byte* bytes = nullptr;
    size_t stride = 0;
};

int elementSizeOf(DType type);

Shape toShape(NSArray<NSNumber*>* dimensions);
NSArray<NSNumber*>* toNSArray(const Shape& shape);

bool toDType(MLMultiArrayDataType dataType, DType& type);

MLMultiArray* nativeArray(const MultiArray& array);

// An array Core ML allocated for an output nobody bound: shared as it is when
// it lies in a pixel buffer, copied into a MultiArray of our own otherwise.
MultiArray adoptOutput(MLMultiArray* output);

// Rows of columns elements from one layout and type to another. Strides are in
// bytes; fp16 <-> fp32 goes through vImage, which honours both strides.
void convertRows(const void* source,
                 DType sourceType,
                 size_t sourceStride,
                 void* destination,
                 DType destinationType,
                 size_t destinationStride,
                 int rows,
                 int columns);
} // namespace eacp::ML
