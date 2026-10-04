#include "ComputeKernel.h"

#include <cassert>
#include <cstring>

namespace eacp::GPU
{
ComputeKernel::ComputeKernel() = default;

ComputeKernel::~ComputeKernel() = default;

ComputeKernel::ComputeKernel(ThreadGroupShape shape)
{
    builder.setThreadGroupShape(shape);
}

const ShaderSource& ComputeKernel::source() const
{
    return generated.source;
}

const ShaderGraph& ComputeKernel::graph() const
{
    return builder.graph();
}

int ComputeKernel::threadgroupMemoryBytes() const
{
    return graph().threadgroupMemoryBytes();
}

const void* ComputeKernel::packedUniforms(int count)
{
    assert(dispatchRank() == DispatchRank::OneD
           && "eacp: only a kernel written against threadId() is dispatched "
              "with dispatch(count)");

    const std::uint32_t extents[] = {(std::uint32_t) count};
    return packWithExtents(extents, 1);
}

const void* ComputeKernel::packedUniforms(int width, int height)
{
    assert(dispatchRank() == DispatchRank::TwoD
           && "eacp: only a kernel written against threadPosition() is "
              "dispatched with dispatch(width, height)");

    const std::uint32_t extents[] = {(std::uint32_t) width, (std::uint32_t) height};
    return packWithExtents(extents, 2);
}

const void* ComputeKernel::packedUniforms(int width, int height, int depth)
{
    assert(dispatchRank() == DispatchRank::ThreeD
           && "eacp: only a kernel written against threadPosition3() is "
              "dispatched with dispatch(width, height, depth)");

    const std::uint32_t extents[] = {
        (std::uint32_t) width, (std::uint32_t) height, (std::uint32_t) depth};
    return packWithExtents(extents, 3);
}

DispatchRank ComputeKernel::dispatchRank() const
{
    return generated.dispatchRank;
}

ThreadGroupShape ComputeKernel::groupShape() const
{
    return builder.threadGroupShape();
}

int ComputeKernel::uniformByteSize() const
{
    return uniformBytes.size();
}

void ComputeKernel::visitMembers(ShaderVisitor& visitor)
{
    reflectMembers(visitor);
}

void ComputeKernel::compile()
{
    auto buildVisitor = ShaderBuildVisitor {builder};
    reflectMembers(buildVisitor);
    define();
    generated = builder.build();
}

UInt ComputeKernel::threadId()
{
    return builder.threadId();
}

ThreadPosition ComputeKernel::threadPosition()
{
    return builder.threadPosition();
}

ThreadPosition3 ComputeKernel::threadPosition3()
{
    return builder.threadPosition3();
}

UInt2 ComputeKernel::threadId2()
{
    return builder.threadId2();
}

UInt3 ComputeKernel::threadId3()
{
    return builder.threadId3();
}

Float ComputeKernel::constant(float value)
{
    return builder.constant(value);
}

UInt ComputeKernel::unsignedInteger(unsigned value)
{
    return builder.unsignedInteger(value);
}

UInt ComputeKernel::localId()
{
    return builder.localId();
}

ThreadPosition ComputeKernel::localPosition()
{
    return builder.localPosition();
}

ThreadPosition3 ComputeKernel::localPosition3()
{
    return builder.localPosition3();
}

UInt ComputeKernel::groupId()
{
    return builder.groupId();
}

ThreadPosition ComputeKernel::groupPosition()
{
    return builder.groupPosition();
}

ThreadPosition3 ComputeKernel::groupPosition3()
{
    return builder.groupPosition3();
}

UInt2 ComputeKernel::localId2()
{
    return builder.localId2();
}

UInt3 ComputeKernel::localId3()
{
    return builder.localId3();
}

UInt2 ComputeKernel::groupId2()
{
    return builder.groupId2();
}

UInt3 ComputeKernel::groupId3()
{
    return builder.groupId3();
}

UInt ComputeKernel::gridCount()
{
    return builder.gridCount();
}

UInt ComputeKernel::gridWidth()
{
    return builder.gridWidth();
}

UInt ComputeKernel::gridHeight()
{
    return builder.gridHeight();
}

UInt ComputeKernel::gridDepth()
{
    return builder.gridDepth();
}

void ComputeKernel::barrier()
{
    builder.barrier();
}

Float ComputeKernel::groupSum(const Float& value)
{
    return builder.groupSum(value);
}

Float ComputeKernel::groupMax(const Float& value)
{
    return builder.groupMax(value);
}

Float ComputeKernel::groupMin(const Float& value)
{
    return builder.groupMin(value);
}

UInt ComputeKernel::groupSum(const UInt& value)
{
    return builder.groupSum(value);
}

UInt ComputeKernel::groupMax(const UInt& value)
{
    return builder.groupMax(value);
}

UInt ComputeKernel::groupMin(const UInt& value)
{
    return builder.groupMin(value);
}

Float ComputeKernel::simdSum(const Float& value)
{
    return builder.simdSum(value);
}

Float ComputeKernel::simdMax(const Float& value)
{
    return builder.simdMax(value);
}

Float ComputeKernel::simdMin(const Float& value)
{
    return builder.simdMin(value);
}

UInt ComputeKernel::simdSum(const UInt& value)
{
    return builder.simdSum(value);
}

UInt ComputeKernel::simdMax(const UInt& value)
{
    return builder.simdMax(value);
}

UInt ComputeKernel::simdMin(const UInt& value)
{
    return builder.simdMin(value);
}

UInt ComputeKernel::simdGroupIndex()
{
    return builder.simdGroupIndex();
}

SimdMatrix ComputeKernel::simdMatrix(float fill)
{
    return builder.simdMatrix(fill);
}

SimdMatrix ComputeKernel::simdMatrix(const Shared<Float>& tile,
                                     const UInt& offset,
                                     const UInt& rowStride)
{
    return builder.simdMatrix(tile, offset, rowStride);
}

SimdMatrix ComputeKernel::simdMatrix(const InputBuffer& buffer,
                                     const UInt& offset,
                                     const UInt& rowStride)
{
    return builder.simdMatrix(buffer, offset, rowStride);
}

SimdMatrix ComputeKernel::simdMatrix(const OutputBuffer& buffer,
                                     const UInt& offset,
                                     const UInt& rowStride)
{
    return builder.simdMatrix(buffer, offset, rowStride);
}

SimdMatrix ComputeKernel::simdMatrixHalf(const InputBuffer& buffer,
                                         const UInt& offset,
                                         const UInt& rowStride)
{
    return builder.simdMatrixHalf(buffer, offset, rowStride);
}

SimdMatrix ComputeKernel::simdMatrixBFloat16(const InputBuffer& buffer,
                                             const UInt& offset,
                                             const UInt& rowStride)
{
    return builder.simdMatrixBFloat16(buffer, offset, rowStride);
}

void ComputeKernel::multiplyAccumulate(const SimdMatrix& accumulator,
                                       const SimdMatrix& left,
                                       const SimdMatrix& right)
{
    builder.multiplyAccumulate(accumulator, left, right);
}

UInt ComputeKernel::atomicAdd(const AtomicBuffer& buffer,
                              const UInt& index,
                              const UInt& value)
{
    return builder.atomicAdd(buffer, index, value);
}

UInt ComputeKernel::atomicAdd(const AtomicBuffer& buffer,
                              unsigned index,
                              const UInt& value)
{
    return builder.atomicAdd(buffer, index, value);
}

UInt ComputeKernel::atomicAdd(const AtomicBuffer& buffer,
                              const UInt& index,
                              unsigned value)
{
    return builder.atomicAdd(buffer, index, value);
}

UInt ComputeKernel::atomicAdd(const AtomicBuffer& buffer,
                              unsigned index,
                              unsigned value)
{
    return builder.atomicAdd(buffer, index, value);
}

Var<Float> ComputeKernel::var(float initialValue)
{
    return builder.var(initialValue);
}

Var<Bool> ComputeKernel::var(bool initialValue)
{
    return builder.var(initialValue);
}

Var<Int> ComputeKernel::var(int initialValue)
{
    return builder.var(initialValue);
}

Var<UInt> ComputeKernel::var(unsigned initialValue)
{
    return builder.var(initialValue);
}

void ComputeKernel::breakLoop()
{
    builder.breakLoop();
}

void ComputeKernel::continueLoop()
{
    builder.continueLoop();
}

void ComputeKernel::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float2& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float3& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float4& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write2(const OutputBuffer& buffer,
                           const UInt& index,
                           const Float2& value)
{
    builder.write2(buffer, index, value);
}

void ComputeKernel::write3(const OutputBuffer& buffer,
                           const UInt& index,
                           const Float3& value)
{
    builder.write3(buffer, index, value);
}

void ComputeKernel::write4(const OutputBuffer& buffer,
                           const UInt& index,
                           const Float4& value)
{
    builder.write4(buffer, index, value);
}

void ComputeKernel::writeHalf2(const OutputBuffer& buffer,
                               const UInt& index,
                               const Float2& value)
{
    builder.writeHalf2(buffer, index, value);
}

void ComputeKernel::writeBFloat16x2(const OutputBuffer& buffer,
                                    const UInt& index,
                                    const Float2& value)
{
    builder.writeBFloat16x2(buffer, index, value);
}

void ComputeKernel::writeInt8x4(const OutputBuffer& buffer,
                                const UInt& index,
                                const Int4& value)
{
    builder.writeInt8x4(buffer, index, value);
}

void ComputeKernel::writeUInt8x4(const OutputBuffer& buffer,
                                 const UInt& index,
                                 const UInt4& value)
{
    builder.writeUInt8x4(buffer, index, value);
}

void ComputeKernel::writeHalf4(const OutputBuffer& buffer,
                               const UInt& index,
                               const Float4& value)
{
    builder.writeHalf4(buffer, index, value);
}

void ComputeKernel::writeBFloat16x4(const OutputBuffer& buffer,
                                    const UInt& index,
                                    const Float4& value)
{
    builder.writeBFloat16x4(buffer, index, value);
}

void ComputeKernel::writeInt8x8(const OutputBuffer& buffer,
                                const UInt& index,
                                const Int4& low,
                                const Int4& high)
{
    builder.writeInt8x8(buffer, index, low, high);
}

void ComputeKernel::writeUInt8x8(const OutputBuffer& buffer,
                                 const UInt& index,
                                 const UInt4& low,
                                 const UInt4& high)
{
    builder.writeUInt8x8(buffer, index, low, high);
}

void ComputeKernel::writeInt8x16(const OutputBuffer& buffer,
                                 const UInt& index,
                                 const Int4& a,
                                 const Int4& b,
                                 const Int4& c,
                                 const Int4& d)
{
    builder.writeInt8x16(buffer, index, a, b, c, d);
}

void ComputeKernel::writeUInt8x16(const OutputBuffer& buffer,
                                  const UInt& index,
                                  const UInt4& a,
                                  const UInt4& b,
                                  const UInt4& c,
                                  const UInt4& d)
{
    builder.writeUInt8x16(buffer, index, a, b, c, d);
}

void ComputeKernel::write(const OutputBuffer& buffer,
                          const UInt& offset,
                          const UInt& rowStride,
                          const SimdMatrix& value)
{
    builder.write(buffer, offset, rowStride, value);
}

void ComputeKernel::write(const Shared<Float>& tile,
                          const UInt& offset,
                          const UInt& rowStride,
                          const SimdMatrix& value)
{
    builder.write(tile, offset, rowStride, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          unsigned value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          unsigned index,
                          const UInt& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          unsigned index,
                          unsigned value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt2& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt3& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt4& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write2(const UIntOutputBuffer& buffer,
                           const UInt& index,
                           const UInt2& value)
{
    builder.write2(buffer, index, value);
}

void ComputeKernel::write3(const UIntOutputBuffer& buffer,
                           const UInt& index,
                           const UInt3& value)
{
    builder.write3(buffer, index, value);
}

void ComputeKernel::write4(const UIntOutputBuffer& buffer,
                           const UInt& index,
                           const UInt4& value)
{
    builder.write4(buffer, index, value);
}

void ComputeKernel::write(const AtomicBuffer& buffer,
                          const UInt& index,
                          const UInt& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const AtomicBuffer& buffer,
                          const UInt& index,
                          unsigned value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const AtomicBuffer& buffer,
                          unsigned index,
                          const UInt& value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const AtomicBuffer& buffer, unsigned index, unsigned value)
{
    builder.write(buffer, index, value);
}

void ComputeKernel::write(const WritableTexture2D& texture,
                          const UInt& x,
                          const UInt& y,
                          const Float4& color)
{
    builder.write(texture, x, y, color);
}

const void* ComputeKernel::packWithExtents(const std::uint32_t* extents, int count)
{
    uniformBytes.clear();
    auto uploadVisitor = ShaderUploadVisitor {uniformBytes};
    reflectMembers(uploadVisitor);

    for (auto i = 0; i < count; ++i)
    {
        auto offset = alignUp(uniformBytes.size(), 4);
        uniformBytes.resize(offset + (int) sizeof(std::uint32_t));
        std::memcpy(uniformBytes.data() + offset, &extents[i], sizeof(extents[i]));
    }

    // After the extents, so the pad lands at the struct's end where MSL
    // puts it, not between the last member and them.
    uploadVisitor.finish();
    return uniformBytes.data();
}
} // namespace eacp::GPU
