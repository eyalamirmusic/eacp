#include "ShaderBuilder.h"

#include "../Pipeline/VertexLayout.h"
#include "ShaderEmitter.h"

namespace eacp::GPU
{
namespace
{
VertexFormat toVertexFormat(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return VertexFormat::Float;
        case ValueType::Float2:
            return VertexFormat::Float2;
        case ValueType::Float3:
            return VertexFormat::Float3;
        case ValueType::Float4:
        case ValueType::Float2x2:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
        case ValueType::UInt:
        case ValueType::UInt2:
        case ValueType::UInt3:
        case ValueType::UInt4:
        case ValueType::Int:
        case ValueType::Int2:
        case ValueType::Int3:
        case ValueType::Int4:
        case ValueType::Bool:
        case ValueType::Bool2:
        case ValueType::Bool3:
        case ValueType::Bool4:
            return VertexFormat::Float4; // matrix/integer/bool are never attributes
    }

    return VertexFormat::Float;
}

VertexLayout buildVertexLayout(const ShaderGraph& graph)
{
    auto layout = VertexLayout {};

    // Group inputs by slot: each slot's attributes accumulate their offsets in
    // declaration order, and the slot's stride is the sum of its byte sizes.
    // Step rate is inherited from the first attribute assigned to a slot -
    // callers who pass explicit bufferIndex are responsible for keeping all
    // attributes in a given slot at the same rate.
    auto perSlotOffsets = Vector<int> {};
    auto perSlotRates = Vector<StepRate> {};
    auto sawInstance = false;

    for (auto i = 0; i < graph.inputs().size(); ++i)
    {
        auto type = graph.inputs()[i];
        auto rate = graph.inputStepRates()[i];
        auto slot = graph.inputBufferIndices()[i];

        while (perSlotOffsets.size() <= slot)
        {
            perSlotOffsets.add(0);
            perSlotRates.add(StepRate::PerVertex);
        }

        // First attribute in a slot establishes its step rate; every later
        // attribute must match. Mixing PerVertex + PerInstance in a single
        // slot would produce a subtly wrong pipeline that each backend
        // resolves differently - a silent cross-platform footgun. Loud in
        // Debug matches the assert-on-unhandled-mode convention added in
        // the BlendModes PR.
        auto firstInSlot = perSlotOffsets[slot] == 0;
        if (firstInSlot)
        {
            perSlotRates[slot] = rate;
        }
        else
        {
            assert(perSlotRates[slot] == rate
                   && "eacp: attributes in a single vertex-buffer slot must "
                      "share a step rate (all PerVertex or all PerInstance)");
        }

        layout.attribute(toVertexFormat(type), perSlotOffsets[slot], slot);
        perSlotOffsets[slot] += byteSize(type);

        if (rate == StepRate::PerInstance)
            sawInstance = true;
    }

    if (sawInstance)
    {
        // Multi-slot layout: publish stride + rate for every slot the graph
        // populated. Empty leading slots (rare) get PerVertex + stride 0 by
        // default, which is a safe no-op for backends.
        for (auto slot = 0; slot < perSlotOffsets.size(); ++slot)
            layout.buffer(slot, perSlotOffsets[slot], perSlotRates[slot]);
    }
    else
    {
        // Single-buffer shortcut: keep the pre-instancing shape (buffers empty,
        // stride populated) so existing single-buffer consumers see no change.
        layout.stride = perSlotOffsets.empty() ? 0 : perSlotOffsets[0];
    }

    return layout;
}
} // namespace

void ShaderBuilder::position(const Float4& clipPosition)
{
    graphData.setPosition(clipPosition.node);
}

void ShaderBuilder::fragment(const Float4& color)
{
    graphData.setFragment(color.node);
}

GeneratedShader ShaderBuilder::build() const
{
    auto source = detail::nativeShaderSource(graphData);

    auto result = GeneratedShader {};

    if (graphData.isCompute())
    {
        source.withCompute("computeMain")
            .withThreadGroup(graphData.threadGroupShape());
        result.source = std::move(source);
        result.dispatchRank = graphData.dispatchRank();
        return result;
    }

    source.withVertex("vertexMain").withFragment("fragmentMain");
    result.source = std::move(source);
    result.vertexLayout = buildVertexLayout(graphData);
    result.vertexReadsUniforms = vertexReadsUniforms(graphData);
    result.fragmentReadsUniforms = fragmentReadsUniforms(graphData);
    return result;
}

Texture2D ShaderBuilder::texture(TextureSampling sampling)
{
    return {&graphData, graphData.addTexture(sampling)};
}

TextureCube ShaderBuilder::cubeTexture(TextureSampling sampling)
{
    return {&graphData, graphData.addCubeTexture(sampling)};
}

TextureDepth2D ShaderBuilder::depthTexture(TextureSampling sampling)
{
    return {&graphData, graphData.addDepthTexture(sampling)};
}

UInt ShaderBuilder::threadId()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addThreadId();
    return value;
}

ThreadPosition ShaderBuilder::threadPosition()
{
    auto position = ThreadPosition {};

    position.x.graph = &graphData;
    position.x.node = graphData.addThreadPosition(0);
    position.y.graph = &graphData;
    position.y.node = graphData.addThreadPosition(1);

    return position;
}

ThreadPosition3 ShaderBuilder::threadPosition3()
{
    auto position = ThreadPosition3 {};

    position.x.graph = &graphData;
    position.x.node = graphData.addThreadPosition3(0);
    position.y.graph = &graphData;
    position.y.node = graphData.addThreadPosition3(1);
    position.z.graph = &graphData;
    position.z.node = graphData.addThreadPosition3(2);

    return position;
}

UInt2 ShaderBuilder::threadId2()
{
    return indexValue<UInt2>(graphData.addThreadId2());
}

UInt3 ShaderBuilder::threadId3()
{
    return indexValue<UInt3>(graphData.addThreadId3());
}

UInt ShaderBuilder::localId()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addLocalId();
    return value;
}

ThreadPosition ShaderBuilder::localPosition()
{
    auto position = ThreadPosition {};

    position.x.graph = &graphData;
    position.x.node = graphData.addLocalPosition(0);
    position.y.graph = &graphData;
    position.y.node = graphData.addLocalPosition(1);

    return position;
}

ThreadPosition3 ShaderBuilder::localPosition3()
{
    auto position = ThreadPosition3 {};

    position.x.graph = &graphData;
    position.x.node = graphData.addLocalPosition3(0);
    position.y.graph = &graphData;
    position.y.node = graphData.addLocalPosition3(1);
    position.z.graph = &graphData;
    position.z.node = graphData.addLocalPosition3(2);

    return position;
}

UInt ShaderBuilder::groupId()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addGroupId();
    return value;
}

ThreadPosition ShaderBuilder::groupPosition()
{
    auto position = ThreadPosition {};

    position.x.graph = &graphData;
    position.x.node = graphData.addGroupPosition(0);
    position.y.graph = &graphData;
    position.y.node = graphData.addGroupPosition(1);

    return position;
}

ThreadPosition3 ShaderBuilder::groupPosition3()
{
    auto position = ThreadPosition3 {};

    position.x.graph = &graphData;
    position.x.node = graphData.addGroupPosition3(0);
    position.y.graph = &graphData;
    position.y.node = graphData.addGroupPosition3(1);
    position.z.graph = &graphData;
    position.z.node = graphData.addGroupPosition3(2);

    return position;
}

UInt2 ShaderBuilder::localId2()
{
    return indexValue<UInt2>(graphData.addLocalId2());
}

UInt3 ShaderBuilder::localId3()
{
    return indexValue<UInt3>(graphData.addLocalId3());
}

UInt2 ShaderBuilder::groupId2()
{
    return indexValue<UInt2>(graphData.addGroupId2());
}

UInt3 ShaderBuilder::groupId3()
{
    return indexValue<UInt3>(graphData.addGroupId3());
}

UInt ShaderBuilder::gridCount()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addGridExtent(DispatchRank::OneD, 0);
    return value;
}

UInt ShaderBuilder::gridWidth()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addGridExtent(DispatchRank::TwoD, 0);
    return value;
}

UInt ShaderBuilder::gridHeight()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addGridExtent(DispatchRank::TwoD, 1);
    return value;
}

UInt ShaderBuilder::gridDepth()
{
    auto value = UInt {};
    value.graph = &graphData;
    value.node = graphData.addGridExtent(DispatchRank::ThreeD, 2);
    return value;
}

void ShaderBuilder::setThreadGroupShape(ThreadGroupShape shape)
{
    graphData.setThreadGroupShape(shape);
}

ThreadGroupShape ShaderBuilder::threadGroupShape() const
{
    return graphData.threadGroupShape();
}

void ShaderBuilder::barrier()
{
    graphData.addBarrier();
}

Float ShaderBuilder::groupSum(const Float& value)
{
    return fold(GroupReduction::Sum, value);
}

Float ShaderBuilder::groupMax(const Float& value)
{
    return fold(GroupReduction::Max, value);
}

Float ShaderBuilder::groupMin(const Float& value)
{
    return fold(GroupReduction::Min, value);
}

UInt ShaderBuilder::groupSum(const UInt& value)
{
    return fold(GroupReduction::Sum, value);
}

UInt ShaderBuilder::groupMax(const UInt& value)
{
    return fold(GroupReduction::Max, value);
}

UInt ShaderBuilder::groupMin(const UInt& value)
{
    return fold(GroupReduction::Min, value);
}

Float ShaderBuilder::simdSum(const Float& value)
{
    return simdFold(GroupReduction::Sum, value);
}

Float ShaderBuilder::simdMax(const Float& value)
{
    return simdFold(GroupReduction::Max, value);
}

Float ShaderBuilder::simdMin(const Float& value)
{
    return simdFold(GroupReduction::Min, value);
}

UInt ShaderBuilder::simdSum(const UInt& value)
{
    return simdFold(GroupReduction::Sum, value);
}

UInt ShaderBuilder::simdMax(const UInt& value)
{
    return simdFold(GroupReduction::Max, value);
}

UInt ShaderBuilder::simdMin(const UInt& value)
{
    return simdFold(GroupReduction::Min, value);
}

UInt ShaderBuilder::simdGroupIndex()
{
    return indexValue<UInt>(graphData.addSimdGroupIndex());
}

SimdMatrix ShaderBuilder::simdMatrix(float fill)
{
    return {&graphData, graphData.addSimdMatrixFill(graphData.addConstant(fill))};
}

SimdMatrix ShaderBuilder::simdMatrix(const Shared<Float>& tile,
                                     const UInt& offset,
                                     const UInt& rowStride)
{
    return {&graphData,
            graphData.addSimdMatrixLoad(
                SimdMatrixMemory::Shared, tile.slot, offset.node, rowStride.node)};
}

SimdMatrix ShaderBuilder::simdMatrix(const InputBuffer& buffer,
                                     const UInt& offset,
                                     const UInt& rowStride)
{
    return {&graphData,
            graphData.addSimdMatrixLoad(
                SimdMatrixMemory::Buffer, buffer.slot, offset.node, rowStride.node)};
}

SimdMatrix ShaderBuilder::simdMatrix(const OutputBuffer& buffer,
                                     const UInt& offset,
                                     const UInt& rowStride)
{
    return {&graphData,
            graphData.addSimdMatrixLoad(
                SimdMatrixMemory::Buffer, buffer.slot, offset.node, rowStride.node)};
}

SimdMatrix ShaderBuilder::simdMatrixHalf(const InputBuffer& buffer,
                                         const UInt& offset,
                                         const UInt& rowStride)
{
    return {&graphData,
            graphData.addSimdMatrixLoad(SimdMatrixMemory::Buffer,
                                        buffer.slot,
                                        offset.node,
                                        rowStride.node,
                                        SimdMatrixElement::Half)};
}

SimdMatrix ShaderBuilder::simdMatrixBFloat16(const InputBuffer& buffer,
                                             const UInt& offset,
                                             const UInt& rowStride)
{
    return {&graphData,
            graphData.addSimdMatrixLoad(SimdMatrixMemory::Buffer,
                                        buffer.slot,
                                        offset.node,
                                        rowStride.node,
                                        SimdMatrixElement::BFloat16)};
}

void ShaderBuilder::multiplyAccumulate(const SimdMatrix& accumulator,
                                       const SimdMatrix& left,
                                       const SimdMatrix& right)
{
    graphData.addSimdMatrixMultiplyAdd(accumulator.slot, left.slot, right.slot);
}

void ShaderBuilder::write(const OutputBuffer& buffer,
                          const UInt& offset,
                          const UInt& rowStride,
                          const SimdMatrix& value)
{
    graphData.addSimdMatrixStore(value.slot,
                                 SimdMatrixMemory::Buffer,
                                 buffer.slot,
                                 offset.node,
                                 rowStride.node);
}

void ShaderBuilder::write(const Shared<Float>& tile,
                          const UInt& offset,
                          const UInt& rowStride,
                          const SimdMatrix& value)
{
    graphData.addSimdMatrixStore(value.slot,
                                 SimdMatrixMemory::Shared,
                                 tile.slot,
                                 offset.node,
                                 rowStride.node);
}

InputBuffer ShaderBuilder::inputBuffer()
{
    return {&graphData, graphData.addStorageBuffer(BufferAccess::Read)};
}

OutputBuffer ShaderBuilder::outputBuffer()
{
    return {&graphData, graphData.addStorageBuffer(BufferAccess::Write)};
}

UIntInputBuffer ShaderBuilder::uintInputBuffer()
{
    return {&graphData,
            graphData.addStorageBuffer(BufferAccess::Read, ValueType::UInt)};
}

UIntOutputBuffer ShaderBuilder::uintOutputBuffer()
{
    return {&graphData,
            graphData.addStorageBuffer(BufferAccess::Write, ValueType::UInt)};
}

AtomicBuffer ShaderBuilder::atomicBuffer()
{
    return {&graphData,
            graphData.addStorageBuffer(BufferAccess::Atomic, ValueType::UInt)};
}

UInt ShaderBuilder::atomicAdd(const AtomicBuffer& buffer,
                              const UInt& index,
                              const UInt& value)
{
    auto previous = graphData.addAtomicAdd(buffer.slot, index.node, value.node);

    auto result = UInt {};
    result.graph = &graphData;
    result.node = graphData.addVarRead(previous);
    return result;
}

UInt ShaderBuilder::atomicAdd(const AtomicBuffer& buffer,
                              unsigned index,
                              const UInt& value)
{
    return atomicAdd(buffer, buffer.literal(index), value);
}

UInt ShaderBuilder::atomicAdd(const AtomicBuffer& buffer,
                              const UInt& index,
                              unsigned value)
{
    return atomicAdd(buffer, index, buffer.literal(value));
}

UInt ShaderBuilder::atomicAdd(const AtomicBuffer& buffer,
                              unsigned index,
                              unsigned value)
{
    return atomicAdd(buffer, buffer.literal(index), buffer.literal(value));
}

WritableTexture2D ShaderBuilder::writableTexture()
{
    return {&graphData, graphData.addWritableTexture()};
}

void ShaderBuilder::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float& value)
{
    graphData.addStore(buffer.slot, index.node, value.node);
}

void ShaderBuilder::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float2& value)
{
    auto base = index * 2u;
    graphData.addRecordStore(buffer.slot,
                             {base.node, (base + 1u).node},
                             {value.x().node, value.y().node},
                             value.node);
}

void ShaderBuilder::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float3& value)
{
    auto base = index * 3u;
    graphData.addRecordStore(buffer.slot,
                             {base.node, (base + 1u).node, (base + 2u).node},
                             {value.x().node, value.y().node, value.z().node},
                             value.node);
}

void ShaderBuilder::write(const OutputBuffer& buffer,
                          const UInt& index,
                          const Float4& value)
{
    auto base = index * 4u;
    graphData.addRecordStore(
        buffer.slot,
        {base.node, (base + 1u).node, (base + 2u).node, (base + 3u).node},
        {value.x().node, value.y().node, value.z().node, value.w().node},
        value.node);
}

void ShaderBuilder::write2(const OutputBuffer& buffer,
                           const UInt& index,
                           const Float2& value)
{
    graphData.addVectorStore(buffer.slot, (index * 2u).node, value.node);
}

void ShaderBuilder::write3(const OutputBuffer& buffer,
                           const UInt& index,
                           const Float3& value)
{
    graphData.addVectorStore(buffer.slot, (index * 3u).node, value.node);
}

void ShaderBuilder::write4(const OutputBuffer& buffer,
                           const UInt& index,
                           const Float4& value)
{
    graphData.addVectorStore(buffer.slot, (index * 4u).node, value.node);
}

void ShaderBuilder::writeHalf2(const OutputBuffer& buffer,
                               const UInt& index,
                               const Float2& value)
{
    write(buffer, index, asFloat(packHalf2(value)));
}

void ShaderBuilder::writeBFloat16x2(const OutputBuffer& buffer,
                                    const UInt& index,
                                    const Float2& value)
{
    write(buffer, index, asFloat(packBFloat16x2(value)));
}

void ShaderBuilder::writeInt8x4(const OutputBuffer& buffer,
                                const UInt& index,
                                const Int4& value)
{
    write(buffer, index, asFloat(packInt8x4(value)));
}

void ShaderBuilder::writeUInt8x4(const OutputBuffer& buffer,
                                 const UInt& index,
                                 const UInt4& value)
{
    write(buffer, index, asFloat(packUInt8x4(value)));
}

void ShaderBuilder::writeHalf4(const OutputBuffer& buffer,
                               const UInt& index,
                               const Float4& value)
{
    write2(buffer,
           index,
           float2(asFloat(packHalf2(value.xy())), asFloat(packHalf2(value.zw()))));
}

void ShaderBuilder::writeBFloat16x4(const OutputBuffer& buffer,
                                    const UInt& index,
                                    const Float4& value)
{
    write2(buffer,
           index,
           float2(asFloat(packBFloat16x2(value.xy())),
                  asFloat(packBFloat16x2(value.zw()))));
}

void ShaderBuilder::writeInt8x8(const OutputBuffer& buffer,
                                const UInt& index,
                                const Int4& low,
                                const Int4& high)
{
    write2(
        buffer, index, float2(asFloat(packInt8x4(low)), asFloat(packInt8x4(high))));
}

void ShaderBuilder::writeUInt8x8(const OutputBuffer& buffer,
                                 const UInt& index,
                                 const UInt4& low,
                                 const UInt4& high)
{
    write2(buffer,
           index,
           float2(asFloat(packUInt8x4(low)), asFloat(packUInt8x4(high))));
}

void ShaderBuilder::writeInt8x16(const OutputBuffer& buffer,
                                 const UInt& index,
                                 const Int4& a,
                                 const Int4& b,
                                 const Int4& c,
                                 const Int4& d)
{
    write4(buffer,
           index,
           float4(asFloat(packInt8x4(a)),
                  asFloat(packInt8x4(b)),
                  asFloat(packInt8x4(c)),
                  asFloat(packInt8x4(d))));
}

void ShaderBuilder::writeUInt8x16(const OutputBuffer& buffer,
                                  const UInt& index,
                                  const UInt4& a,
                                  const UInt4& b,
                                  const UInt4& c,
                                  const UInt4& d)
{
    write4(buffer,
           index,
           float4(asFloat(packUInt8x4(a)),
                  asFloat(packUInt8x4(b)),
                  asFloat(packUInt8x4(c)),
                  asFloat(packUInt8x4(d))));
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt& value)
{
    graphData.addStore(buffer.slot, index.node, value.node);
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          unsigned value)
{
    write(buffer, index, buffer.literal(value));
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          unsigned index,
                          const UInt& value)
{
    write(buffer, buffer.literal(index), value);
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          unsigned index,
                          unsigned value)
{
    write(buffer, buffer.literal(index), buffer.literal(value));
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt2& value)
{
    auto base = index * 2u;
    graphData.addRecordStore(buffer.slot,
                             {base.node, (base + 1u).node},
                             {value.x().node, value.y().node},
                             value.node);
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt3& value)
{
    auto base = index * 3u;
    graphData.addRecordStore(buffer.slot,
                             {base.node, (base + 1u).node, (base + 2u).node},
                             {value.x().node, value.y().node, value.z().node},
                             value.node);
}

void ShaderBuilder::write(const UIntOutputBuffer& buffer,
                          const UInt& index,
                          const UInt4& value)
{
    auto base = index * 4u;
    graphData.addRecordStore(
        buffer.slot,
        {base.node, (base + 1u).node, (base + 2u).node, (base + 3u).node},
        {value.x().node, value.y().node, value.z().node, value.w().node},
        value.node);
}

void ShaderBuilder::write2(const UIntOutputBuffer& buffer,
                           const UInt& index,
                           const UInt2& value)
{
    graphData.addVectorStore(buffer.slot, (index * 2u).node, value.node);
}

void ShaderBuilder::write3(const UIntOutputBuffer& buffer,
                           const UInt& index,
                           const UInt3& value)
{
    graphData.addVectorStore(buffer.slot, (index * 3u).node, value.node);
}

void ShaderBuilder::write4(const UIntOutputBuffer& buffer,
                           const UInt& index,
                           const UInt4& value)
{
    graphData.addVectorStore(buffer.slot, (index * 4u).node, value.node);
}

void ShaderBuilder::write(const AtomicBuffer& buffer,
                          const UInt& index,
                          const UInt& value)
{
    graphData.addStore(buffer.slot, index.node, value.node);
}

void ShaderBuilder::write(const AtomicBuffer& buffer,
                          const UInt& index,
                          unsigned value)
{
    write(buffer, index, buffer.literal(value));
}

void ShaderBuilder::write(const AtomicBuffer& buffer,
                          unsigned index,
                          const UInt& value)
{
    write(buffer, buffer.literal(index), value);
}

void ShaderBuilder::write(const AtomicBuffer& buffer, unsigned index, unsigned value)
{
    write(buffer, buffer.literal(index), buffer.literal(value));
}

void ShaderBuilder::write(const WritableTexture2D& texture,
                          const UInt& x,
                          const UInt& y,
                          const Float4& color)
{
    graphData.addTextureStore(texture.slot, x.node, y.node, color.node);
}

detail::ValueHandle ShaderBuilder::addVertexInput(ValueType type)
{
    return {&graphData, graphData.addInput(type)};
}

detail::ValueHandle ShaderBuilder::addInstanceInput(ValueType type, int bufferIndex)
{
    return {&graphData, graphData.addInstanceInput(type, bufferIndex)};
}

detail::ValueHandle ShaderBuilder::addUniform(ValueType type)
{
    return {&graphData, graphData.addUniform(type)};
}

Float ShaderBuilder::constant(float value)
{
    auto result = Float {};
    result.graph = &graphData;
    result.node = graphData.addConstant(value);
    return result;
}

Bool ShaderBuilder::boolean(bool value)
{
    auto result = Bool {};
    result.graph = &graphData;
    result.node = graphData.addBoolConstant(value);
    return result;
}

Int ShaderBuilder::integer(int value)
{
    auto result = Int {};
    result.graph = &graphData;
    result.node = graphData.addIntConstant(value);
    return result;
}

UInt ShaderBuilder::unsignedInteger(unsigned value)
{
    auto result = UInt {};
    result.graph = &graphData;
    result.node = graphData.addUIntConstant(value);
    return result;
}

Var<Float> ShaderBuilder::var(float initialValue)
{
    return {graphData, ValueType::Float, graphData.addConstant(initialValue)};
}

Var<Bool> ShaderBuilder::var(bool initialValue)
{
    return {graphData, ValueType::Bool, graphData.addBoolConstant(initialValue)};
}

Var<Int> ShaderBuilder::var(int initialValue)
{
    return {graphData, ValueType::Int, graphData.addIntConstant(initialValue)};
}

Var<UInt> ShaderBuilder::var(unsigned initialValue)
{
    return {graphData, ValueType::UInt, graphData.addUIntConstant(initialValue)};
}

void ShaderBuilder::breakLoop()
{
    graphData.addBreak();
}

void ShaderBuilder::continueLoop()
{
    graphData.addContinue();
}

void ShaderBuilder::discardBelow(const Float& value, float threshold)
{
    graphData.setDiscard(value.node, threshold);
}
} // namespace eacp::GPU
