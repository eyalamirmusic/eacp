#pragma once

#include "../Frame/ComputePass.h"
#include "KernelName.h"
#include "ShaderMembers.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

// The device-free half of a struct-authored compute kernel: the members, the
// recorded body, and the source and graph generated from it. ComputeProgram
// derives from it and adds the device - the library, the pipeline and the
// bind - so a kernel written against ComputeKernel instead is declared exactly
// as the ScaleKernel in ComputeProgram.h is, records and emits the same graph,
// and links against eacp-gpu-codegen alone. It cannot be prepared or dispatched
// on a GPU; that is what ComputeProgram is for.

namespace eacp::GPU
{
// Base for struct-authored compute kernels. Derive, declare uniform and buffer
// members, list them with EACP_SHADER, write define(), and call compile() from
// the constructor.
class ComputeKernel
{
public:
    ComputeKernel();

    // The threadgroup this kernel is dispatched in, in place of the stock shape
    // for its rank: ComputeProgram({256}) over a 1D grid, ComputeProgram({16,
    // 16}) over a 2D one. The body reads it back through groupShape().
    explicit ComputeKernel(ThreadGroupShape shape);

    virtual ~ComputeKernel();

    // Members point into the owned builder's graph and the GPU resources are
    // non-copyable, so a program is pinned in place (like ShaderProgram).
    ComputeKernel(const ComputeKernel&) = delete;
    ComputeKernel& operator=(const ComputeKernel&) = delete;

    const ShaderSource& source() const;

    // What a per-dispatch timing calls this kernel: its type's name without
    // namespaces, "LinearF32". Override it to tell apart the variants one type
    // builds.
    virtual std::string name() const { return readableTypeName(typeid(*this)); }

    // The graph the body was recorded into, so either backend's text can be
    // emitted from the kernel that ships rather than from a copy of its body.
    const ShaderGraph& graph() const;

    // How many bytes of threadgroup memory one group of this kernel takes. It
    // counts the emitter's own reduction and matrix scratch beside the shared<>
    // arrays, and takes the worst case of the three backends rather than this
    // one's - a kernel is written once and has to fit everywhere it runs.
    int threadgroupMemoryBytes() const;

    // Re-packs the current uniform values, appends the element count the
    // generated bounds guard reads, and returns the block, ready for
    // ComputePass::setBytes.
    const void* packedUniforms(int count);

    // The 2D sibling: the grid extents the two-dimensional guard reads, in the
    // order the emitted block declares them.
    const void* packedUniforms(int width, int height);

    // And the 3D one, whose guard reads three.
    const void* packedUniforms(int width, int height, int depth);

    // The grid shape this kernel's body asked for, which decides which dispatch
    // it takes.
    DispatchRank dispatchRank() const;

    // This kernel's own group, which is what localId() runs to and what a
    // shared tile is sized in. Read it inside define() after the body has asked
    // for its thread index, since an unset shape resolves against the rank.
    ThreadGroupShape groupShape() const;

    // The stock group shape, which is what a kernel that named none is
    // dispatched in: groupWidth threads in a 1D kernel, groupSize2D squared in
    // a 2D one, groupSize3D cubed in a 3D one.
    static constexpr int groupWidth = ComputePass::threadGroupWidth;
    static constexpr int groupSize2D = ComputePass::threadGroupSize2D;
    static constexpr int groupSize3D = ComputePass::threadGroupSize3D;

    // How many threads one SIMD group holds, and the side of a SimdMatrix
    // fragment: what a kernel divides its group into blocks by, and what its
    // tiles are multiples of.
    static constexpr int simdWidth = simdGroupWidth;
    static constexpr int simdMatrixWidth = simdMatrixSize;

    int uniformByteSize() const;

    // Walks the members EACP_SHADER lists with a visitor of the caller's own,
    // which is how a CPU executor reads the current uniform values.
    void visitMembers(ShaderVisitor& visitor);

protected:
    // Runs the member build walk (adopting uniform and buffer slots), the
    // user's define(), then emits the kernel source. Called from the
    // most-derived constructor.
    void compile();

    UInt threadId();
    ThreadPosition threadPosition();
    ThreadPosition3 threadPosition3();

    // The same work item as one value: a UInt2 over a grid, a UInt3 over a
    // volume, fixing the rank exactly as the two above do.
    UInt2 threadId2();
    UInt3 threadId3();
    Float constant(float value);
    UInt unsignedInteger(unsigned value);

    // The threadgroup vocabulary, forwarded on the terms the ids above set:
    // where a thread sits in its group, which group it is in, the implicit
    // grid bound the dispatch supplied, a shared array, and the barrier that
    // orders access to it. Shared tiles are sized against groupShape(), which
    // is the group the dispatch really runs.
    UInt localId();
    ThreadPosition localPosition();
    ThreadPosition3 localPosition3();
    UInt groupId();
    ThreadPosition groupPosition();
    ThreadPosition3 groupPosition3();

    // Their whole-vector forms, beside threadId2() and threadId3().
    UInt2 localId2();
    UInt3 localId3();
    UInt2 groupId2();
    UInt3 groupId3();

    UInt gridCount();
    UInt gridWidth();
    UInt gridHeight();
    UInt gridDepth();
    void barrier();

    template <typename T>
    Shared<T> shared(int count)
    {
        return builder.shared<T>(count);
    }

    // The fold of what every thread of the group contributed, returned to
    // every thread. It barriers, so - like barrier() - every thread of the
    // group has to reach it or none of them.
    Float groupSum(const Float& value);
    Float groupMax(const Float& value);
    Float groupMin(const Float& value);

    UInt groupSum(const UInt& value);
    UInt groupMax(const UInt& value);
    UInt groupMin(const UInt& value);

    // The same fold narrowed to one SIMD group: every thread is handed the
    // fold of the simdWidth threads it shares one with, so a group of several
    // SIMD groups comes out holding one answer per SIMD group rather than one
    // for the group. Collective on the terms the group version sets, and on
    // Metal a single instruction with neither scratch nor a barrier - see
    // ShaderBuilder.
    Float simdSum(const Float& value);
    Float simdMax(const Float& value);
    Float simdMin(const Float& value);

    UInt simdSum(const UInt& value);
    UInt simdMax(const UInt& value);
    UInt simdMin(const UInt& value);

    // The SIMD-group matrix vocabulary: which SIMD group a thread is in, an
    // 8x8 float fragment filled or loaded, and the multiply-accumulate over
    // three of them. A fragment is written back through write(), beside the
    // element writes. See ShaderBuilder for what each takes, and SimdMatrix
    // for what one is.
    UInt simdGroupIndex();

    SimdMatrix simdMatrix(float fill = 0.f);

    SimdMatrix simdMatrix(const Shared<Float>& tile,
                          const UInt& offset,
                          const UInt& rowStride);

    SimdMatrix simdMatrix(const InputBuffer& buffer,
                          const UInt& offset,
                          const UInt& rowStride);

    SimdMatrix simdMatrix(const OutputBuffer& buffer,
                          const UInt& offset,
                          const UInt& rowStride);

    // The packed siblings: an 8x8 patch of fp16 or bf16 read straight out of a
    // device buffer, the offset and the row stride counting in those
    // sixteen-bit elements. What a kernel saves by taking one is the tile it
    // would otherwise widen a weight into and the two barriers around it. See
    // ShaderBuilder for the rules, and fitsPackedSimdMatrix for the question to
    // put to the device before recording one.
    SimdMatrix simdMatrixHalf(const InputBuffer& buffer,
                              const UInt& offset,
                              const UInt& rowStride);

    SimdMatrix simdMatrixBFloat16(const InputBuffer& buffer,
                                  const UInt& offset,
                                  const UInt& rowStride);

    void multiplyAccumulate(const SimdMatrix& accumulator,
                            const SimdMatrix& left,
                            const SimdMatrix& right);

    // Adds to one element of a shared counter and yields what it held before, so
    // threads that never meet each other still come away with distinct numbers.
    // See ShaderBuilder::atomicAdd for what it does and does not order.
    UInt atomicAdd(const AtomicBuffer& buffer, const UInt& index, const UInt& value);

    UInt atomicAdd(const AtomicBuffer& buffer, unsigned index, const UInt& value);

    UInt atomicAdd(const AtomicBuffer& buffer, const UInt& index, unsigned value);

    UInt atomicAdd(const AtomicBuffer& buffer, unsigned index, unsigned value);

    // Control flow, forwarded from the builder on the terms ShaderProgram
    // forwards it: a mutable local, the two branching statements, the loop and
    // its two jumps. The unsigned overload is the counter a reduction kernel
    // walks a buffer with - it lives beside the UInt indices threadId() hands
    // out, and the uint comparisons are what bound it.
    template <ShaderHandleLike T>
    Var<ShaderHandle<T>> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    Var<Float> var(float initialValue);
    Var<Bool> var(bool initialValue);
    Var<Int> var(int initialValue);
    Var<UInt> var(unsigned initialValue);

    template <typename Body>
    void ifThen(const Bool& condition, Body&& body)
    {
        builder.ifThen(condition, std::forward<Body>(body));
    }

    template <typename Then, typename Else>
    void ifThen(const Bool& condition, Then&& whenTrue, Else&& whenFalse)
    {
        builder.ifThen(
            condition, std::forward<Then>(whenTrue), std::forward<Else>(whenFalse));
    }

    template <typename Body>
    void loop(const Bool& condition, Body&& body)
    {
        builder.loop(condition, std::forward<Body>(body));
    }

    void breakLoop();
    void continueLoop();

    void write(const OutputBuffer& buffer, const UInt& index, const Float& value);

    // The vector writes, for a buffer whose elements are records of N floats.
    // The index is in records, so it pairs with InputBuffer::read2/3/4 and a
    // kernel never spells the stride itself.
    void write(const OutputBuffer& buffer, const UInt& index, const Float2& value);

    void write(const OutputBuffer& buffer, const UInt& index, const Float3& value);

    void write(const OutputBuffer& buffer, const UInt& index, const Float4& value);

    // The same records laid down as one store rather than as N - the write
    // mirror of read2/read3/read4, down to the index counting records and the
    // four-byte alignment a packed vector pointer asks of the binding. See
    // ShaderBuilder::write4 for why this is a name of its own.
    void write2(const OutputBuffer& buffer, const UInt& index, const Float2& value);

    void write3(const OutputBuffer& buffer, const UInt& index, const Float3& value);

    void write4(const OutputBuffer& buffer, const UInt& index, const Float4& value);

    // Two values narrowed to fp16 and packed into the one float slot that
    // holds them, which InputBuffer::readHalf2 reads back at the same index.
    void writeHalf2(const OutputBuffer& buffer,
                    const UInt& index,
                    const Float2& value);

    // Two values narrowed to bf16 and packed into the one float slot that holds
    // them, which InputBuffer::readBFloat16x2 reads back at the same index.
    void writeBFloat16x2(const OutputBuffer& buffer,
                         const UInt& index,
                         const Float2& value);

    // Four integers packed into the one float slot that holds them, which
    // InputBuffer::readInt8x4 and readUInt8x4 read back at the same index.
    void writeInt8x4(const OutputBuffer& buffer,
                     const UInt& index,
                     const Int4& value);

    void writeUInt8x4(const OutputBuffer& buffer,
                      const UInt& index,
                      const UInt4& value);

    // The wide packed stores, one per wide read and at the read's own index:
    // eight or sixteen values packed into the two or four words that hold them
    // and laid down in one store. The byte ones take integer vectors for the
    // reason writeInt8x4 does, named after the read's own .low / .high and
    // .a .b .c .d.
    void writeHalf4(const OutputBuffer& buffer,
                    const UInt& index,
                    const Float4& value);

    void writeBFloat16x4(const OutputBuffer& buffer,
                         const UInt& index,
                         const Float4& value);

    void writeInt8x8(const OutputBuffer& buffer,
                     const UInt& index,
                     const Int4& low,
                     const Int4& high);

    void writeUInt8x8(const OutputBuffer& buffer,
                      const UInt& index,
                      const UInt4& low,
                      const UInt4& high);

    void writeInt8x16(const OutputBuffer& buffer,
                      const UInt& index,
                      const Int4& a,
                      const Int4& b,
                      const Int4& c,
                      const Int4& d);

    void writeUInt8x16(const OutputBuffer& buffer,
                       const UInt& index,
                       const UInt4& a,
                       const UInt4& b,
                       const UInt4& c,
                       const UInt4& d);

    // One element of a threadgroup-shared array, published to the rest of the
    // group by the next barrier().
    template <typename T>
    void write(const Shared<T>& array, const UInt& index, const T& value)
    {
        builder.write(array, index, value);
    }

    // An 8x8 fragment written back to the patch a load reads: element (r, c)
    // of it at offset + r * rowStride + c, and the whole patch inside the
    // array. See ShaderBuilder and SimdMatrix.
    void write(const OutputBuffer& buffer,
               const UInt& offset,
               const UInt& rowStride,
               const SimdMatrix& value);

    void write(const Shared<Float>& tile,
               const UInt& offset,
               const UInt& rowStride,
               const SimdMatrix& value);

    // One element of an integer output: the id or the count a kernel arrived
    // at, kept as an integer for the kernel after it to index with.
    void write(const UIntOutputBuffer& buffer, const UInt& index, const UInt& value);

    void write(const UIntOutputBuffer& buffer, const UInt& index, unsigned value);

    void write(const UIntOutputBuffer& buffer, unsigned index, const UInt& value);

    void write(const UIntOutputBuffer& buffer, unsigned index, unsigned value);

    // The record writes, for a buffer whose elements are records of N integers.
    void
        write(const UIntOutputBuffer& buffer, const UInt& index, const UInt2& value);

    void
        write(const UIntOutputBuffer& buffer, const UInt& index, const UInt3& value);

    void
        write(const UIntOutputBuffer& buffer, const UInt& index, const UInt4& value);

    // The same records laid down as one store, on the terms the float wide
    // stores set and at the index UIntInputBuffer::read2/3/4 counts in.
    void write2(const UIntOutputBuffer& buffer,
                const UInt& index,
                const UInt2& value);

    void write3(const UIntOutputBuffer& buffer,
                const UInt& index,
                const UInt3& value);

    void write4(const UIntOutputBuffer& buffer,
                const UInt& index,
                const UInt4& value);

    // An atomic buffer's element, set rather than added to - what a kernel
    // computing a dispatch size writes.
    void write(const AtomicBuffer& buffer, const UInt& index, const UInt& value);

    void write(const AtomicBuffer& buffer, const UInt& index, unsigned value);

    void write(const AtomicBuffer& buffer, unsigned index, const UInt& value);

    void write(const AtomicBuffer& buffer, unsigned index, unsigned value);

    // One texel of a kernel's output image, at the coordinates a 2D kernel
    // already has in hand from threadPosition().
    void write(const WritableTexture2D& texture,
               const UInt& x,
               const UInt& y,
               const Float4& color);

    // Generated by EACP_SHADER: visits each declared member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    // Written by the user: the kernel body.
    virtual void define() = 0;

private:
    const void* packWithExtents(const std::uint32_t* extents, int count);

    ShaderBuilder builder;
    GeneratedShader generated;
    Vector<std::byte> uniformBytes;
};
} // namespace eacp::GPU
