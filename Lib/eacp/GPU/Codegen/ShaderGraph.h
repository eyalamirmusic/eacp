#pragma once

/** @file ShaderGraph, the backend-neutral expression and statement tree the EDSL
    records into.
*/

#include "../Common.h"

#include "ShaderTypes.h"

#include "../Pipeline/VertexLayout.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Texture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace eacp::GPU
{
enum class ExprKind
{
    Input, // vertex attribute; index = attribute slot
    Varying, // fragment-stage read of a varying; index = varying slot
    Uniform, // per-frame constant; index = field slot in the uniform block
    Constant, // scalar float literal; value
    Construct, // floatN(args...); args = child nodes. Of type Struct, a struct
    // value built from its members in order (structType says which)
    Swizzle, // child.<components>; args[0] = child
    Call, // builtin call text(args...); e.g. sin/cos. The emitter translates
    // the canonical (MSL) name where HLSL spells it differently.
    Unary, // (op child); args = {child}; op. Negation, logical and bitwise not.
    Binary, // (lhs op rhs); args = {lhs, rhs}. The operator is `op` when it fits
    // in a char and `text` when it does not - which is only the two shifts.
    Compare, // (lhs op rhs) yielding a Bool; args = {lhs, rhs}, op text in `text`.
    // Separate from Binary for two reasons: <=, == and && do not fit in a char,
    // and the result is a Bool whatever shape the operands are.
    Select, // (condition ? whenTrue : whenFalse); args = {condition, a, b}. Both
    // languages spell the conditional operator the same way.
    VarRead, // the current value of a mutable local; index = variable slot.
    // Unlike every other node this is not a pure expression: what it evaluates
    // to depends on which statements have run, so the emitter never hoists one
    // past an assignment (see the per-statement local planning in the emitter).
    // On a private array it is one element: args = {the index, already
    // clamped to the array}.
    Mul, // a matrix product: args = {left, right} in the order written, which
    // is matrix * vector, vector * matrix or matrix * matrix. Emits per-backend
    // (MSL uses the * operator, HLSL uses mul()), so it is not a plain Binary -
    // and both languages read whichever operand is on the left the same way, so
    // the order is the whole of what distinguishes the three.
    Sample, // texture sample; index = texture slot, args = {uv} or {uv, level}.
    // Emits per-backend (MSL t.sample(s, uv), HLSL t.Sample(s, uv)). On an
    // array slot the second argument is the layer instead, and a comparison
    // sample is text "compare" with the reference depth as its last argument.
    Fetch, // texel read at integer coordinates, no sampler; index = texture slot,
    // args = {coordinates}. Emits per-backend (MSL t.read(), HLSL t.Load()).
    ThreadId, // compute work-item id; emitted as the kernel's gid parameter.
    // index is the component: a 1D kernel has only 0 and prints the whole gid,
    // a 2D or 3D one prints gid.x, gid.y or gid.z - or the whole gid again at
    // allComponents, where the node is the position as one vector.
    BufferRead, // storage-buffer element read; index = buffer slot, args = {index}
    BufferVectorRead, // a run of 2, 3 or 4 consecutive elements of a read-only
    // storage buffer, taken as one vector. index = buffer slot, args = {the
    // *first element's* index}, type = the vector. Separate from a Construct
    // over that many BufferReads because Metal reinterprets the pointer and
    // makes one load of it, where the other two have no spelling for that and
    // emit exactly the componentwise construct this stands in for. Read-only
    // buffers only - see InputBuffer::read4 for why an output stays scalar.
    AtomicLoad, // one element of an atomic buffer; index = buffer slot,
    // args = {index}. An expression on both backends, unlike the add - MSL
    // spells it atomic_load_explicit and HLSL is an ordinary subscript, since
    // there a UAV element is already what an interlocked op works on.
    ArrayRead, // constant-array element read; index = array slot, args = {index}
    LocalId, // position within the threadgroup; index = component, like ThreadId
    GroupId, // the threadgroup's own index in the grid; index = component
    GridExtent, // the implicit bounds uniform the generated guard reads: count
    // for a 1D kernel, width/height/depth by component otherwise. Exposed so a
    // kernel that barriers - and therefore has no early-return guard - can
    // bound its stores against the very same value the dispatch supplied.
    SharedRead, // threadgroup-array element read; index = slot, args = {index}
    SimdGroupIndex, // which SIMD group of the threadgroup this thread is in.
    // Metal's own builtin; the flat local index divided by simdGroupWidth
    // where there is no builtin, which is the same number.
    BuiltIn, // a value a stage is handed rather than computes; index = the
    // ShaderBuiltIn.
    TextureFunction, // one of WGSL's texture functions past the plain sample;
    // index = texture slot, text = the function (see textureFunctionName),
    // args = {coordinate, the layer on a layered kind, then the function's
    // own: a level, a bias, two gradients or a reference}, op = the component
    // a colour gather reads ('x'..'w').
    TextureQuery, // textureDimensions, textureNumLevels or textureNumLayers;
    // index = texture slot, text = "size", "levels" or "layers", args = {the
    // level} where a size names one.
    MatrixColumn, // one column of a float matrix; args = {matrix, an Int index
    // already clamped to the columns}. MSL and GLSL subscript the matrix; HLSL,
    // whose subscript is a row, subscripts its transpose.
    Override, // a pipeline-overridable constant, WGSL's override; index = its
    // id (ShaderGraph::overrideConstant). A scalar the pipeline supplies.
    FunctionCall // a call of a function the shader declares (ShaderGraph::
    // beginFunction); index = the function, args = the arguments in order, a
    // struct one being its Construct. A value read where it is printed when the
    // function may be; otherwise the value of a Call statement, which names it.
};

// The values a stage is handed rather than computes. The first six belong to a
// render stage and the last four to a kernel; which stage may read one is
// stageOf's answer, and the emitters assert it.
enum class ShaderBuiltIn
{
    FragmentCoordinate,
    VertexId,
    InstanceId,
    FrontFacing,
    SampleIndex,
    SampleMask,
    LocalIndex,
    GroupCount,
    SimdLaneIndex,
    SimdGroupSize
};

constexpr ShaderStage stageOf(ShaderBuiltIn which)
{
    switch (which)
    {
        case ShaderBuiltIn::VertexId:
        case ShaderBuiltIn::InstanceId:
            return ShaderStage::Vertex;

        case ShaderBuiltIn::FragmentCoordinate:
        case ShaderBuiltIn::FrontFacing:
        case ShaderBuiltIn::SampleIndex:
        case ShaderBuiltIn::SampleMask:
            return ShaderStage::Fragment;

        case ShaderBuiltIn::LocalIndex:
        case ShaderBuiltIn::GroupCount:
        case ShaderBuiltIn::SimdLaneIndex:
        case ShaderBuiltIn::SimdGroupSize:
            return ShaderStage::Compute;
    }

    return ShaderStage::Fragment;
}

constexpr ValueType builtInType(ShaderBuiltIn which)
{
    switch (which)
    {
        case ShaderBuiltIn::FragmentCoordinate:
            return ValueType::Float4;
        case ShaderBuiltIn::FrontFacing:
            return ValueType::Bool;
        case ShaderBuiltIn::GroupCount:
            return ValueType::UInt3;
        case ShaderBuiltIn::VertexId:
        case ShaderBuiltIn::InstanceId:
        case ShaderBuiltIn::SampleIndex:
        case ShaderBuiltIn::SampleMask:
        case ShaderBuiltIn::LocalIndex:
        case ShaderBuiltIn::SimdLaneIndex:
        case ShaderBuiltIn::SimdGroupSize:
            return ValueType::UInt;
    }

    return ValueType::UInt;
}

// WGSL's @interpolate(type, sampling) on a varying. Perspective-correct at the
// pixel centre is what a varying gets unasked; Linear drops the perspective
// divide, Flat takes one vertex's value. Centroid moves the point inside the
// primitive where it covers only part of a pixel, and Sample runs the fragment
// stage once per sample, each at its own position. A Flat varying takes First
// or Either and nothing else: the value is the first vertex's of each primitive
// on every backend, so Either is printed as First is.
enum class InterpolationType : std::uint8_t
{
    Perspective,
    Linear,
    Flat
};

enum class InterpolationSampling : std::uint8_t
{
    Center,
    Centroid,
    Sample,
    First,
    Either
};

struct Interpolation
{
    InterpolationType type = InterpolationType::Perspective;
    InterpolationSampling sampling = InterpolationSampling::Center;

    static constexpr Interpolation
        perspective(InterpolationSampling sampling = InterpolationSampling::Center)
    {
        return {InterpolationType::Perspective, sampling};
    }

    static constexpr Interpolation
        linear(InterpolationSampling sampling = InterpolationSampling::Center)
    {
        return {InterpolationType::Linear, sampling};
    }

    static constexpr Interpolation
        flat(InterpolationSampling sampling = InterpolationSampling::First)
    {
        return {InterpolationType::Flat, sampling};
    }

    constexpr bool operator==(const Interpolation& other) const = default;
};

constexpr bool isFlatSampling(InterpolationSampling sampling)
{
    return sampling == InterpolationSampling::First
           || sampling == InterpolationSampling::Either;
}

// What WGSL accepts: flat with first or either, the other two with center,
// centroid or sample, and an integer varying only flat.
constexpr bool isValidInterpolation(ValueType type, Interpolation interpolation)
{
    const auto isFlat = interpolation.type == InterpolationType::Flat;

    if (isFlat != isFlatSampling(interpolation.sampling))
        return false;

    return isFlat || !(isSignedInteger(type) || isUnsignedInteger(type));
}

constexpr Interpolation defaultInterpolation(ValueType type)
{
    if (isSignedInteger(type) || isUnsignedInteger(type))
        return Interpolation::flat();

    return Interpolation::perspective();
}

// How a kernel accesses a storage buffer: a read-only input (Metal device
// const / D3D SRV), a writable output (Metal device / D3D UAV), or an atomic
// one - elements every thread may read-modify-write at once.
//
// What the elements are is the separate question addStorageBuffer's element
// type answers, on the terms TextureAccess and TextureKind divide a texture's:
// a Read or a Write slot holds floats or unsigned integers, and an Atomic one
// is unsigned integers wrapped in the type an interlocked operation acts
// through.
enum class BufferAccess
{
    Read,
    Write,
    Atomic
};

// How a shader accesses a texture: sampled and fetched (Metal
// texture2d<float>, D3D Texture2D through an SRV) or written by a kernel
// (Metal access::write, D3D RWTexture2D through a UAV). Both kinds take slots
// from one counter, because Metal binds them to one texture index space.
//
// Read and ReadWrite are the storage textures of StorageTextures.h: a texel
// load with no sampler through the read binding (an SRV, a sampled image), and
// a load and a store through the write one. Write, Read and ReadWrite slots
// carry the format they were declared with (ShaderGraph::textureFormat).
//
// Sampler is no texture at all: a sampler declared as a binding of its own
// (Samplers.h), which takes its slot from the same counter so its sampler
// index, register and binding are ones no texture's baked sampler holds.
enum class TextureAccess
{
    Sample,
    Write,
    Read,
    ReadWrite,
    Sampler
};

constexpr bool isStorageAccess(TextureAccess access)
{
    return access == TextureAccess::Write || access == TextureAccess::Read
           || access == TextureAccess::ReadWrite;
}

// Whether a storage texture may be declared in this format: every format a
// kernel can be asked to store to, which is every uncompressed one but
// BGRA8Unorm, the sRGB ones and RGB9E5Float (supportsComputeWrite's refusals).
constexpr bool isStorageTextureFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::BGRA8UnormSrgb:
        case TextureFormat::RGB9E5Float:
        case TextureFormat::BC1RGBA:
        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
        case TextureFormat::BC1RGBASrgb:
        case TextureFormat::BC2RGBASrgb:
        case TextureFormat::BC3RGBASrgb:
        case TextureFormat::BC7RGBASrgb:
        case TextureFormat::BC4RUnorm:
        case TextureFormat::BC4RSnorm:
        case TextureFormat::BC5RGUnorm:
        case TextureFormat::BC5RGSnorm:
        case TextureFormat::BC6HRGBUfloat:
            return false;
        default:
            return true;
    }
}

// What a storage texture of this format loads and stores, WGSL's way: four
// components whatever the format holds, of the format's own scalar.
constexpr ValueType storageTexelType(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::R8Uint:
        case TextureFormat::RG8Uint:
        case TextureFormat::RGBA8Uint:
        case TextureFormat::R16Uint:
        case TextureFormat::RG16Uint:
        case TextureFormat::RGBA16Uint:
        case TextureFormat::R32Uint:
        case TextureFormat::RG32Uint:
        case TextureFormat::RGBA32Uint:
            return ValueType::UInt4;
        case TextureFormat::R8Sint:
        case TextureFormat::RG8Sint:
        case TextureFormat::RGBA8Sint:
        case TextureFormat::R16Sint:
        case TextureFormat::RG16Sint:
        case TextureFormat::RGBA16Sint:
        case TextureFormat::R32Sint:
        case TextureFormat::RG32Sint:
        case TextureFormat::RGBA32Sint:
            return ValueType::Int4;
        default:
            return ValueType::Float4;
    }
}

// What shape a texture slot is: a 2D image sampled with a float2, or six square
// faces sampled with a float3 direction. It rides beside TextureAccess rather
// than inside it because the two answer different questions - one is how the
// shader reaches the texture, the other is what the texture is - and only the
// declaration the emitter prints depends on this one.
//
// The sample itself does not. `t.sample(s, uv)` on Metal and `t.Sample(s, uv)`
// on HLSL are how both kinds are read, with the coordinate's own width deciding
// which; so the emitter's Sample case is untouched by cube textures, and the
// only place a kind is read is where the parameter or the global is declared.
// A depth slot is the one kind where the sample expression's *type* differs
// rather than only its declaration: `depth2d<float>` on Metal and
// `Texture2D<float>` on HLSL both hand back one float where the other two hand
// back four. So this is read in two places rather than one - the declaration,
// and the node addDepthSample gives the sample.
enum class TextureKind
{
    Texture2D,
    Cube,

    // The depth buffer of a render target, bound with
    // RenderPass::setFragmentDepthTexture. Not a texture the app created: a
    // depth attachment belongs to the target it was made with, which is why the
    // bind takes the target rather than a texture of its own.
    Depth2D,

    // The layered forms of Texture2D and Depth2D, sampled with a layer beside
    // the coordinate: a TwoDArray texture, or the whole depth plane of a
    // TwoDArray target bound with setFragmentDepthTexture.
    Texture2DArray,
    Depth2DArray,

    // A ThreeD texture, an array of cubes, and the depth plane of a Cube or a
    // CubeArray target. Read only through the texture functions below.
    Texture3D,
    CubeArray,
    DepthCube,
    DepthCubeArray
};

constexpr bool isLayered(TextureKind kind)
{
    return kind == TextureKind::Texture2DArray || kind == TextureKind::Depth2DArray
           || kind == TextureKind::CubeArray || kind == TextureKind::DepthCubeArray;
}

constexpr bool isDepth(TextureKind kind)
{
    return kind == TextureKind::Depth2D || kind == TextureKind::Depth2DArray
           || kind == TextureKind::DepthCube || kind == TextureKind::DepthCubeArray;
}

constexpr bool isCubeKind(TextureKind kind)
{
    return kind == TextureKind::Cube || kind == TextureKind::CubeArray
           || kind == TextureKind::DepthCube || kind == TextureKind::DepthCubeArray;
}

// What a TextureFunction node does. Sample, Bias and Compare take the level
// from the fragment's derivatives and are fragment-stage only; the rest run in
// any stage that declares textures.
enum class TextureFunction
{
    Sample,
    Level,
    Bias,
    Grad,
    Compare,
    CompareLevel,
    Gather,
    GatherCompare,
    Load
};

constexpr bool usesImplicitDerivatives(TextureFunction function)
{
    return function == TextureFunction::Sample || function == TextureFunction::Bias
           || function == TextureFunction::Compare;
}

constexpr bool isComparisonFunction(TextureFunction function)
{
    return function == TextureFunction::Compare
           || function == TextureFunction::CompareLevel
           || function == TextureFunction::GatherCompare;
}

std::string_view textureFunctionName(TextureFunction function);
TextureFunction textureFunctionNamed(std::string_view name);

// Which fold a group-wide reduction performs over the value every thread of
// the group contributed.
enum class GroupReduction
{
    Sum,
    Max,
    Min
};

// How many threads a reduction folds over: the whole threadgroup, or only the
// SIMD group the folding thread belongs to.
//
// The narrow one is what a kernel wants wherever its partials are already one
// per SIMD group - the tile loop of an attention kernel, say, where the whole
// group has nothing to say to each other yet. On Metal the difference is one
// instruction against a scratch array between two threadgroup barriers.
enum class ReductionScope
{
    Group,
    Simd
};

// Which of the two address spaces a statement reaches: a threadgroup array the
// group staged, or a storage buffer the dispatch bound. A SIMD-group matrix
// fragment is loaded from and stored to either, and an atomic acts on either.
enum class MemorySpace
{
    Shared,
    Buffer
};

using SimdMatrixMemory = MemorySpace;

// The read-modify-write an Atomic statement performs. Every one yields the
// value the element held before it; Max and Min compare as the element type
// does, signed for an Int element. CompareExchange stores only when the element
// equals the comparand, and says whether it did.
enum class AtomicOperation
{
    Add,
    Sub,
    Max,
    Min,
    And,
    Or,
    Xor,
    Exchange,
    CompareExchange
};

// What the elements of a loaded fragment are in the memory it comes out of.
// Float is the buffer's own elements; the two packed ones are sixteen bits
// each, two to a word, and the offset and the row stride of such a load count
// in those elements rather than in the words holding them - the convention
// InputBuffer::readHalf and readBFloat16 already set.
//
// A packed fragment is an operand and nothing else. Metal multiplies one
// straight into a float accumulator, which is the whole point of loading one;
// it has no instruction that stores one, and an accumulator in sixteen bits
// would lose the precision a product is accumulated in. The EDSL offers no way
// to ask for either, and the graph asserts on both.
//
// Whether a device loads one natively is Device::supportsHalfSimdMatrix and
// Device::supportsBFloat16SimdMatrix, asked before the kernel is written; the
// two backends that answer no still build such a load, widening each lane's
// pair by hand. See ComputeProgram::simdMatrixBFloat16.
enum class SimdMatrixElement
{
    Float,
    Half,
    BFloat16
};

// How many threads one SIMD group holds - the width the matrix ops are
// collective over. 32 on every Apple GPU, which is the only hardware whose
// intrinsics are used; the backends that emit the scalar fallback define
// theirs to be the same number so a kernel's tiling arithmetic is one
// arithmetic everywhere.
inline constexpr int simdGroupWidth = 32;

// One side of an 8x8 fragment. Fixed, because that is the only shape MSL's
// simdgroup_float8x8 has.
inline constexpr int simdMatrixSize = 8;

// The shape of the grid a kernel is dispatched over, decided by which thread
// index its body asked for: threadId() gives one index over a flat count,
// threadPosition() a pair over a width and a height, threadPosition3() a triple
// over a volume. The emitter takes the entry signature and the bounds guard
// from this, and the dispatch takes the grid from the matching
// ComputePass::dispatch overload - which is why a kernel asks for one of them.
enum class DispatchRank
{
    OneD,
    TwoD,
    ThreeD
};

// The component a ThreadId, LocalId or GroupId node carries when it is the
// whole position rather than one lane of it.
inline constexpr int allComponents = -1;

// What a statement does. Statements are what the expression store on its own
// cannot say: that one value is computed before another, that a value changes,
// and that a run of them repeats or is skipped. The control statements are
// spelled alike in every dialect but Discard, which Metal calls
// discard_fragment().
enum class StatementKind
{
    Declare, // <type> vN = value; slot = variable, value = its initial value.
    // A private array's declaration sets every element to value
    Assign, // vN = value; slot = variable, value = the expression assigned.
    // index, where set, is the private-array element written (clamped)
    If, // if (value) { body } else { elseBody }
    Loop, // while (value) { body } - and, where continuing names a block,
    // WGSL's continuing { ... }: run after the body and after every continue,
    // before value is tested again
    Break,
    Continue,
    BreakIf, // WGSL's break if: leaves the loop when value holds. Only the last
    // statement of a continuing block
    Switch, // switch (value) on an Int or UInt; body = a block of Case
    // statements and nothing else. No clause falls through
    Case, // one clause of a switch: slot = its labels (ShaderGraph::caseLabels),
    // body = the block it runs
    Return, // leaves the function: a kernel's invocation ends, and a vertex or
    // fragment one returns its outputs as they evaluate at this point
    Discard, // the fragment writes nothing, colour and depth alike
    Store, // buffer[index] = value; slot = the storage slot
    VectorStore, // buffer[index .. index + N - 1] = value; slot = the storage
    // slot, index = the *first element's* index, value = the vector stored. The
    // write mirror of ExprKind::BufferVectorRead, and one store for the same
    // reason: Metal reinterprets the pointer at the address it is storing to,
    // which retypes the access and not the binding.
    TextureStore, // texture[index, indexY] = value; slot = the texture slot.
    // A storage texture's store has indexY = -1, index = the coordinate
    // vector and layer = the array layer (or -1).
    SharedStore, // shared[index] = value; slot = the threadgroup-array slot
    Barrier, // threadgroup barrier: every thread in the group arrives before
    // any proceeds, and threadgroup memory written before it is visible after
    GroupReduce, // vN = the fold of `value` over the whole threadgroup,
    // declaring vN. slot = the variable the result lands in, reduction = which
    // fold. A statement for the reason the atomic add is one and then some: it
    // is several statements on every backend - a barrier among them - so it has
    // to land where it was written, and every thread of the group has to reach
    // it or none.
    SimdMatrixFill, // an 8x8 fragment declared and filled with one value.
    // slot = the fragment, value = what every element is set to.
    SimdMatrixLoad, // an 8x8 fragment declared and read from an 8x8 patch.
    // slot = the fragment, memory / bufferSlot = where from, index = the
    // element the patch starts at, stride = the patch's row stride, element =
    // what those elements are in memory.
    SimdMatrixStore, // that patch written back. The same fields, the other way.
    SimdMatrixMultiplyAdd, // slot = slot + left * right, all three fragments.
    // slot = the accumulator, left / right = the operands.
    Atomic, // vN = atomic<op>(memory[index], value), declaring vN. slot = the
    // variable the value *before* the operation lands in, atomic = which
    // operation, memory / bufferSlot / index = which element (a storage buffer
    // or a threadgroup array), value = the operand. A CompareExchange also
    // names compare = the comparand and exchanged = a Bool variable it declares.
    //
    // A statement rather than an expression, and it is the one place the
    // languages force that: MSL's atomic_fetch_add_explicit returns the old
    // value, but HLSL's InterlockedAdd writes it through an out parameter and
    // cannot appear in the middle of one. Naming the result is the only shape
    // all of them can print, and it is the shape a caller wants anyway - the
    // old value is a slot reserved for this thread, which is what the whole
    // operation is usually for.
    Call // vN = function(args...), declaring vN, or function(args...) alone:
    // value = the FunctionCall node, slot = the variable its result lands in,
    // -1 where it has none. In the graph withFunctionsInlined makes for the CPU
    // executor it is the call's body instead: body = the block, run as a
    // function's - a Return in it leaves that block alone - and value -1.
};

// One statement. Which fields carry meaning depends on the kind above; the
// bodies are block indices so a statement stays plain data of a fixed size and
// the graph owns every block, exactly as it owns every expression node.
struct Statement
{
    StatementKind kind = StatementKind::Assign;
    int slot = -1; // Declare / Assign: the variable written; stores: the slot
    int value = -1; // Declare / Assign / stores: the value; If / Loop / BreakIf:
    // the condition; Switch: the selector
    int body = -1; // If / Loop / Case: the block that runs; Switch: its clauses
    int elseBody = -1; // If: the block that runs when the condition is false
    int continuing = -1; // Loop: the block run before the condition is retested
    int index = -1; // Store: the element index; VectorStore: the *first*
    // element's index, the rest of the record following it; TextureStore: x;
    // Atomic: the element
    int indexY = -1; // TextureStore: y
    int layer = -1; // TextureStore of a layered storage texture: the layer
    int bufferSlot = -1; // Atomic: the buffer or the shared array, its slot
    // field being taken by the variable the old value lands in
    AtomicOperation atomic = AtomicOperation::Add; // Atomic: which operation
    int compare = -1; // Atomic CompareExchange: the comparand
    int exchanged = -1; // Atomic CompareExchange: the Bool variable it declares
    int record = -1; // Store: the record this element is a component of, -1
    // where the store is a write of its own
    int recordComponentsLeft = 0; // Store: how many components of that record
    // follow this one
    GroupReduction reduction = GroupReduction::Sum; // GroupReduce: which fold
    ReductionScope scope = ReductionScope::Group; // GroupReduce: over how many
    int stride = -1; // SimdMatrixLoad / SimdMatrixStore: the patch's row stride
    int left = -1; // SimdMatrixMultiplyAdd: the left operand's fragment
    int right = -1; // SimdMatrixMultiplyAdd: the right operand's fragment
    MemorySpace memory = MemorySpace::Shared; // which address space a
    // SimdMatrixLoad / SimdMatrixStore or an Atomic reaches, bufferSlot being
    // the slot in it
    SimdMatrixElement element = SimdMatrixElement::Float; // SimdMatrixLoad:
    // what the patch's elements are in memory, and so what the fragment is
    int member = -1; // Assign to a struct variable, Store to a struct buffer:
    // the field written (ShaderGraph::structType), -1 for the whole value
};

// What a block is the body of, for the statements only some may hold: a jump
// out of a loop needs a loop body, break-if a continuing block, and a switch's
// clause list holds Case statements alone.
enum class BlockRole
{
    Plain,
    LoopBody,
    Continuing,
    Cases
};

// A run of statements, held by index so a nested body is an int on the
// statement that owns it.
struct Block
{
    Vector<int> statements; // indices into the graph's statement store
    BlockRole role = BlockRole::Plain;
};

// The labels of one switch clause: integer literals of the selector's type,
// held wide so an Int and a UInt label fit alike, and whether it is the
// default clause.
struct CaseLabels
{
    Vector<std::int64_t> values;
    bool isDefault = false;
};

// A constant array the shader subscripts: the palette a procedural shader picks
// a colour out of, the offsets a sampling kernel walks. It lives beside the
// expression store rather than in it because an array is a declaration and not
// a value - the one thing a shader names that no single node stands for. Its
// elements are ordinary expressions, evaluated once where the array is declared
// at the top of the shader body, so they may read uniforms and varyings but not
// a mutable local, which does not exist yet at that point.
struct ArrayConstant
{
    ValueType elementType = ValueType::Float;
    Vector<int> elements; // expression nodes, one per element
};

// A threadgroup-shared array: the tile a reduction or a blocked matmul stages
// in on-chip memory. Unlike a storage buffer it never crosses the CPU
// boundary, so its element type is whatever the kernel wants - a float4 tile
// is one wide element, not four scalars with a layout contract. The size is a
// compile-time constant in the emitted source, fixed when define() runs.
//
// An atomic one holds UInt or Int elements declared in the type an atomic
// operation acts through - Metal's atomic_uint / atomic_int - and is read and
// stored through atomic loads and stores there; HLSL and GLSL declare it as
// plain integers.
struct SharedArray
{
    ValueType elementType = ValueType::Float;
    int elements = 0;
    bool atomic = false;
};

// One node in the shader expression tree. Plain data referenced by integer id so
// value handles stay trivially copyable and the graph owns every node.
struct Expr
{
    ExprKind kind = ExprKind::Constant;
    ValueType type = ValueType::Float;
    int index = 0; // Input / Varying / Uniform slot; value of a UInt Constant
    float value = 0.0f; // Float Constant
    char op = '+'; // Binary
    std::string text; // Swizzle components ("xy") or Call name ("sin")
    Vector<int> args; // child node ids
    int sampler = -1; // TextureFunction: the sampler slot it samples through,
    // -1 for the texture's own baked sampling
    int structType = -1; // a Construct of type Struct: which struct it builds
    int member = -1; // a BufferRead of a struct buffer, a VarRead of a struct
    // variable: the field read, always one that is not itself a struct
};

// One member of a struct a shader declares (ShaderStruct.h): its name, its
// type - ValueType::Struct and which struct for a nested one - and its offset
// in WGSL's storage layout.
struct StructMember
{
    std::string name;
    ValueType type = ValueType::Float;
    int structType = -1;
    int offset = 0;
};

// A member at any depth, in the order a walk meets them that takes a nested
// struct before its own members, addressed from the outermost struct: its
// path ("inner.x"), its offset from the element's start, the run of the
// struct's flat components it holds, and which struct declares the member.
// A nested struct's fields follow it, so its run covers theirs.
struct StructField
{
    std::string path;
    ValueType type = ValueType::Float;
    int structType = -1;
    int offset = 0;
    int firstComponent = 0;
    int componentCount = 0;
    int owner = -1;
};

// One scalar of a struct laid out flat, every field's components in field
// order: its byte offset in an element and whether it is an f16.
struct StructComponent
{
    int offset = 0;
    bool half = false;
};

// A struct type: what its members are, and WGSL's storage size and alignment
// of it. hostShareable is false where a Bool is in it at any depth: such a
// struct is a value and never the element of a buffer. fields and components
// are the graph's to fill when the struct is added.
struct StructType
{
    std::string name;
    Vector<StructMember> members;
    int size = 0;
    int alignment = 1;
    bool hostShareable = true;
    Vector<StructField> fields;
    Vector<StructComponent> components;
};

// A constant the pipeline supplies, WGSL's override: a Bool, Int, UInt, Float
// or Half scalar named in the shader, with or without a default. Its id is its
// place in declaration order, which is what each backend keys it by.
// defaultBits holds the default as PipelineConstant::bits does.
struct OverrideDeclaration
{
    std::string name;
    ValueType type = ValueType::Float;
    bool hasDefault = false;
    std::uint32_t defaultBits = 0;
    int node = -1;
};

// A function the shader declares, WGSL's fn (ShaderFunction.h): its name, its
// parameters - variables of its own, which the call's arguments initialise and
// nothing assigns - its result, none where returnsValue is false, and the
// root block its body was recorded into, which ends in the Return of the
// result. What the body does, the functions it calls counted in:
// writesStorage for a buffer or texture store or an atomic, readsMutableStorage
// for a read of a buffer or texture a kernel may write, discards for a
// discard. isPure where it reads nothing that can change but its own
// variables, which makes a call of it as pure as its arguments.
struct FunctionDeclaration
{
    std::string name;
    Vector<int> parameters;
    ValueType returnType = ValueType::Bool;
    int returnStruct = -1;
    bool returnsValue = false;
    int body = -1;
    Vector<int> callees;
    bool writesStorage = false;
    bool readsMutableStorage = false;
    bool discards = false;
    bool isPure = false;
    bool isComplete = false;
};

// Backend-agnostic shader IR: an expression-node store plus the shader's I/O
// (ordered vertex inputs, ordered varyings, the clip-space position expression
// and the fragment-output expression). The same node list drives both the
// emitted source and the vertex layout, so a shader and its layout cannot drift.
// Built by ShaderBuilder, read by the emitters; never uses runtime reflection.
class ShaderGraph
{
public:
    ShaderGraph();
    ~ShaderGraph();
    ShaderGraph(const ShaderGraph& other);
    ShaderGraph(ShaderGraph&& other) noexcept;
    ShaderGraph& operator=(const ShaderGraph& other);
    ShaderGraph& operator=(ShaderGraph&& other) noexcept;

    struct VaryingSlot
    {
        ValueType type = ValueType::Float;
        int sourceNode = -1; // vertex-stage expression feeding this varying
        Interpolation interpolation;
    };

    // One kernel output write: buffer[index] = value. Recording any store
    // marks the whole graph as a compute kernel, the way position/fragment
    // mark a render one - this list is that signature. Each store is also
    // recorded as a statement in the block open at the time, which is where
    // it is emitted: a write inside a loop body runs once per iteration,
    // not once after the loop.
    struct Store
    {
        int slot = -1;
        int index = -1;
        int value = -1;
    };

    // Its texture sibling: texture[x, y] = colour. A compute signature entry
    // exactly as a buffer store is, and what makes a kernel able to produce
    // something a later render pass samples.
    struct TextureStore
    {
        int slot = -1;
        int x = -1;
        int y = -1;
        int value = -1;
        int layer = -1;
    };

    int addInput(ValueType type);

    // A per-instance input. Emitted shader source is identical to a per-vertex
    // input; the split shows up only in the emitted VertexLayout, which routes
    // instance inputs to a dedicated buffer slot with PerInstance step rate.
    // The zero-arg form auto-assigns slot 1 (the common case: one
    // per-instance buffer alongside the per-vertex buffer at slot 0). Pass an
    // explicit bufferIndex when a shader needs multiple per-instance streams
    // in distinct buffers (e.g. per-instance transform in slot 1, per-instance
    // colour in slot 2).
    int addInstanceInput(ValueType type);
    int addInstanceInput(ValueType type, int bufferIndex);
    int addVarying(ValueType type, int sourceNode);
    int addVarying(ValueType type, int sourceNode, Interpolation interpolation);
    int addUniform(ValueType type);

    // The names a struct-authored program's members give its uniforms, buffers,
    // textures and samplers (EACP_SHADER), kept for reflection alone: nothing
    // emitted reads them, and a slot no member named has an empty one.
    void nameUniform(int slot, const std::string& name);
    void nameStorageBuffer(int slot, const std::string& name);
    void nameTexture(int slot, const std::string& name);
    const std::string& uniformName(int slot) const;
    const std::string& storageBufferName(int slot) const;
    const std::string& textureName(int slot) const;
    int addConstant(float value);

    // An f16 literal, held already rounded to the nearest half (ties to even,
    // past 65504 an infinity), so every backend and the CPU read one value.
    int addHalfConstant(float value);
    int addUIntConstant(unsigned value);
    int addIntConstant(int value);
    int addBoolConstant(bool value);
    int addConstruct(ValueType type, Vector<int> args);
    int addSwizzle(ValueType type, int child, std::string components);
    int addCall(ValueType type, std::string name, int argument);
    int addCall(ValueType type, std::string name, Vector<int> args);
    int addUnary(ValueType type, char op, int child);
    int addBinary(ValueType type, char op, int lhs, int rhs);

    // The same node for an operator a char cannot hold, which is the two shifts
    // and nothing else. Kept off addCompare, whose result is a Bool whatever it
    // was given: a shift is shaped like the value being shifted.
    int addBinary(ValueType type, std::string op, int lhs, int rhs);

    int addCompare(std::string op, int lhs, int rhs);

    // The componentwise form, whose result is a boolean of the operands' width
    // rather than a scalar. Both languages give `<` on two vectors exactly this,
    // so the node prints the same way the scalar one does.
    int addCompare(ValueType type, std::string op, int lhs, int rhs);
    int addSelect(ValueType type, int condition, int whenTrue, int whenFalse);
    int addMul(ValueType type, int left, int right);

    // Mutable locals and the statements that drive them. A variable is declared
    // where it is created, so the statement stream is also its scope: creating
    // one inside a loop body declares it there, and the C++ handle that names it
    // goes out of scope at the same brace.
    //
    // Statements append to the block on top of the stack. pushBlock/popBlock
    // bracket the body of an if or a loop; the block they leave behind is what
    // addIf/addLoop then names.
    int addVariable(ValueType type, int initialValue);
    int addVarRead(int slot);
    void assign(int slot, int value);

    // WGSL's var a: array<T, N>: a private array of `size` elements, declared
    // with every element set to `fill`, its elements read and written by an
    // index. The index is an Int or a UInt; it is clamped into [0, size) here,
    // as graph nodes, so every backend and the CPU executor read and write the
    // same element for one past either end.
    int addArrayVariable(ValueType elementType, int size, int fill);
    int addVarElementRead(int slot, int index);
    void assignElement(int slot, int index, int value);

    // The elements of variable `slot`: 0 for a plain variable.
    int variableArraySize(int slot) const;

    // WGSL's structs (ShaderStruct.h). addStruct declares a struct type, its
    // nested structs already added, and fills its fields and components; a
    // second struct of a name already declared is that struct, and has to
    // match it. A struct value is a Construct of its members in order - a
    // nested struct's a Construct of its own - and reaches a variable, a
    // private array's element or a buffer's whole; a member is read as a
    // field, which a VarRead or a BufferRead names, and written as one by an
    // Assign or a Store naming it.
    int addStruct(const StructType& type);
    const StructType& structType(int id) const;
    int structCount() const;
    int addStructConstruct(int structType, const Vector<int>& members);

    // The struct's zero value, WGSL's: every member's zero.
    int addStructZero(int structType);

    int addStructVariable(int structType, int initialValue);
    int addStructArrayVariable(int structType, int size, int fill);

    // Which struct variable `slot` holds, -1 when it holds no struct.
    int variableStructType(int slot) const;

    // WGSL's override: a scalar constant the pipeline supplies, declared once
    // by name and read as one node. A second declaration of a name already
    // declared is that constant, and has to agree with it in type and
    // default. The name has to be an identifier.
    int addOverride(const std::string& name, ValueType type);
    int addOverride(const std::string& name,
                    ValueType type,
                    std::uint32_t defaultBits);
    constexpr const Vector<OverrideDeclaration>& overrides() const
    {
        return overrideList;
    }

    // The id of the override of that name, -1 when there is none.
    int findOverride(std::string_view name) const;

    // A field of a struct variable, or of an element of a private array of
    // them; and an assignment to a field, a nested struct's included. The
    // element's index is one clampVariableIndex already clamped, so one index
    // serves every field of the element.
    int clampVariableIndex(int slot, int index);
    int addVarMemberRead(int slot, int field);
    int addVarElementMemberRead(int slot, int index, int field);
    void assignMember(int slot, int field, int value);
    void assignElementMember(int slot, int index, int field, int value);

    // A storage buffer whose elements are a host-shareable struct, a field of
    // element `index` and a store to one - `field` -1 storing the element
    // whole. Only a Write slot is stored to.
    int addStructBuffer(BufferAccess access, int structType);
    int storageStructType(int slot) const;
    int addBufferMemberRead(int slot, int index, int field);
    void addMemberStore(int slot, int index, int field, int value);

    // What a field holds, as a node assigned or stored into it must: its
    // ValueType, and which struct where that is ValueType::Struct.
    bool fitsField(int structType, int field, int value) const;

    // The zero of any value type, WGSL's zero value: a literal for a scalar, a
    // construct of them for a vector, of zero columns for a matrix.
    int addZero(ValueType type);

    // Column `index` of a matrix (an Int, clamped here like an array's).
    int addMatrixColumn(int matrix, int index);

    int pushBlock(BlockRole role = BlockRole::Plain);
    void popBlock();

    void addIf(int condition, int body, int elseBody);
    void addLoop(int condition, int body, int continuing = -1);
    void addBreak();
    void addContinue();

    // WGSL's break if, the last statement of a loop's continuing block.
    void addBreakIf(int condition);

    // A switch over an Int or UInt selector. Its clauses are Case statements
    // recorded into a BlockRole::Cases block, each naming labels of the
    // selector's type - no value twice, at most one default - and its body.
    void addCase(const CaseLabels& labels, int body);
    void addSwitch(int selector, int cases);
    const CaseLabels& caseLabels(int clause) const;
    int caseCount() const;

    void addReturn();

    // The fragment stage's discard, as many times and wherever its statements
    // are; the vertex stage and a kernel have none.
    void addDiscard();

    // The vertex stage's statements. What is recorded between beginVertexStage
    // and endVertexStage lands in a root block of its own, emitted into the
    // vertex function ahead of the position and the varyings; everything else
    // lands in rootBlock, the fragment's or the kernel's. A variable belongs to
    // the stage it was declared in: only that stage assigns or reads it, and a
    // vertex one reaches the fragment through a varying. Only the variable and
    // control statements may be recorded here, discard excepted - WGSL's vertex
    // stage has no storage writes, atomics or barriers. Not nested, and not
    // opened inside a block.
    void beginVertexStage();
    void endVertexStage();

    // WGSL's user functions (ShaderFunction.h). beginFunction opens the root
    // block a function's body is recorded into, in place of every block open,
    // and returns the function's id; addParameter makes a parameter, a
    // variable of the function; endFunction closes the body - adding the
    // Return of `result` where the function has one - checks what it reaches
    // and puts back what was open. A body reads its parameters, its own
    // variables and what has module scope - literals, overrides, uniforms,
    // buffers, textures and samplers - and nothing a stage is handed: no
    // built-in, vertex input, varying, thread index, shared memory or constant
    // array, which are passed to it as arguments instead. No function is
    // declared inside another, and one calls only functions completed before
    // it, so none recurses. The name is an identifier no other function has.
    int beginFunction(const std::string& name,
                      ValueType returnType,
                      int returnStruct,
                      bool returnsValue);
    int addParameter(ValueType type, int structType = -1);
    void endFunction(int result);

    // A call of `function`. Where the function may be read as a value - it
    // returns one that is not a struct, and neither writes nor reads storage a
    // kernel may write, nor discards - it is a FunctionCall node, evaluated
    // wherever it is read. Otherwise it is a Call statement in the block open,
    // its result landing in a variable it declares: node is a read of that
    // variable, a struct's being read a field at a time.
    struct FunctionResult
    {
        int node = -1;
        int variable = -1;
    };

    FunctionResult addFunctionCall(int function, const Vector<int>& arguments);

    // A function's return of `value`, which a function with a result returns
    // from anywhere in its body; returnEarly() there is a void function's.
    void addReturn(int value);

    const FunctionDeclaration& function(int id) const;
    int functionCount() const;
    constexpr int recordingFunctionId() const { return recordingFunction; }

    // Whether function `id` is called as a statement rather than read.
    bool isCalledAsStatement(int id) const;

    // Which function variable `slot` belongs to, -1 where it is a stage's.
    int variableFunction(int slot) const;

    // This graph with every call replaced by the body it calls, which is what
    // the CPU executor plans: a Call statement with the body copied into it,
    // its parameters declared from the arguments, a Return in it assigning the
    // result. A call read as a value is made, in front of each statement
    // reading it, into such a statement and a read of its result; a loop whose
    // condition reads one tests it at the top of its body. Nothing else
    // changes, so a graph with no function is returned as it is.
    ShaderGraph withFunctionsInlined() const;

    // Registers a 2D texture slot (always a float-returning texture2d, so only
    // the slot index and how it is sampled are stored), and a sample of it at a
    // float2 coordinate - with the mip level the hardware picks from the
    // derivatives, or with one the shader chooses.
    int addTexture(TextureSampling sampling = {});
    int addSample(int textureSlot, int uv);
    int addSample(int textureSlot, int uv, int level);

    // A cube slot, from the same counter and the same sampler space as the 2D
    // one - a shader that declares both binds them at distinct indices, and
    // RenderPass::setFragmentTexture takes either at either. What it changes is
    // the declaration the emitter prints and the width of the coordinate
    // addSample is handed; nothing else here knows the difference.
    int addCubeTexture(TextureSampling sampling = {});

    // A depth slot, again from the same counter and the same sampler space, and
    // the sample of one - which is a Float rather than a Float4, both backends'
    // depth textures having exactly one channel to give. That is the whole
    // reason this is its own pair of entry points rather than a flag on the two
    // above: the node's type is what every expression built on it reads.
    int addDepthTexture(TextureSampling sampling = {});
    int addDepthSample(int textureSlot, int uv);

    // The array forms, whose sample takes a layer beside the coordinate. The
    // node is a Float4 on a colour array and a Float on a depth one.
    int addArrayTexture(TextureSampling sampling = {});
    int addDepthArrayTexture(TextureSampling sampling = {});
    int addLayerSample(int textureSlot, int uv, int layer);

    // A comparison sample of a depth slot declared with TextureCompare: one
    // Float in 0..1, filtered across the footprint where the sampling is linear.
    int addCompareSample(int textureSlot, int uv, int reference);
    int addCompareSample(int textureSlot, int uv, int layer, int reference);

    int addBuiltIn(ShaderBuiltIn which);

    // A texture slot a kernel writes rather than reads, and one such write. It
    // takes a slot from the same counter addTexture does, so a kernel that
    // reads one texture and writes another binds them at distinct indices -
    // which is what Metal's single texture index space requires.
    int addWritableTexture();
    void addTextureStore(int slot, int x, int y, int value);

    // A storage texture of a 2D, 2D-array or 3D kind (StorageTextures.h), and
    // its load and store at a coordinate vector, with the layer of an array
    // or -1. The load is the slot format's storageTexelType.
    int addStorageTexture(TextureKind kind,
                          TextureFormat format,
                          TextureAccess access);
    int addStorageLoad(int slot, int coordinates, int layer);
    void addStorageStore(int slot, int coordinates, int layer, int value);

    // One texel read straight out of the texture at integer coordinates: no
    // sampler, so no filtering, no addressing and no interpolation.
    int addFetch(int textureSlot, int coordinates);

    // A slot of any kind, for the kinds with no entry point of their own above.
    int addTextureOfKind(TextureKind kind, TextureSampling sampling = {});

    // A sampler slot (TextureAccess::Sampler), from the texture counter, and
    // whether one is a comparison sampler.
    int addSampler(bool comparison);
    bool isComparisonSampler(int slot) const;

    // A TextureFunction node. args are the coordinate, the layer where the
    // slot's kind is layered, then the function's own; a colour gather reads
    // `component` (0 to 3). The node is a Float4, or a Float where a depth
    // kind's sample, level or load gives one channel and every comparison.
    // samplerSlot names a sampler slot to sample through in place of the
    // texture's own sampling; a comparison function takes a comparison one.
    int addTextureFunction(int textureSlot,
                           TextureFunction function,
                           const Vector<int>& args,
                           int component = 0,
                           int samplerSlot = -1);

    // A TextureQuery node: "size" (a UInt2, a UInt3 on a Texture3D) at `level`,
    // or with no level for level 0; "levels" or "layers" (a UInt) with none.
    int addTextureQuery(int textureSlot, const std::string& query, int level = -1);

    // A constant array and a subscript of one. Reading past the end is
    // undefined in both shading languages exactly as it is in GLSL, so an index
    // a shader has not already bounded is worth masking or clamping.
    int addArray(ValueType elementType, Vector<int> elements);
    int addArrayRead(int slot, int index);

    // Compute kernel pieces: the 1D work-item id, one component of the 2D or 3D
    // one, a storage-buffer slot (inputs and outputs share one slot space, so
    // every buffer gets a distinct index), an element read, and an element
    // write. The first thread index a kernel asks for fixes its dispatch rank,
    // and asking for another one afterwards is a contradiction the emitted
    // kernel could not express.
    //
    // The element type is Float or UInt, and it is what a read of the slot
    // yields.
    int addThreadId();
    int addThreadPosition(int component);
    int addThreadPosition3(int component);

    // The same work item as one vector node rather than a component of one,
    // typed UInt2 or UInt3. Fixes the rank as the component forms do.
    int addThreadId2();
    int addThreadId3();
    int addStorageBuffer(BufferAccess access,
                         ValueType elementType = ValueType::Float);
    int addBufferRead(int slot, int index);

    // A run of consecutive elements of a read-only buffer as one vector, the
    // index being the first element's rather than the record's. Metal makes one
    // load of it; the other two spell the componentwise construct it stands for.
    int addBufferVectorRead(int slot, int firstElement, ValueType type);

    void addStore(int slot, int index, int value);

    // A run of consecutive elements written as one vector, the index being the
    // first element's rather than the record's - addBufferVectorRead run
    // backwards. Metal makes one store of it; the other two spell the N
    // subscripts it stands for, over a value named once beforehand.
    void addVectorStore(int slot, int firstElement, int value);

    // The N element stores one record write lays down, told apart from N
    // writes of their own: a record is one write above, so every component of
    // it takes the value the record had before the first of them ran.
    void addRecordStore(int slot,
                        const Vector<int>& indices,
                        const Vector<int>& components,
                        int record);

    // The atomics. addAtomic returns the *variable* slots holding the element's
    // value from before the operation and, for a CompareExchange, whether it
    // stored - which addVarRead then reads. It is a statement, so unlike every
    // other producer here it does not yield a node. The slot is an Atomic
    // storage buffer or an atomic shared array, by `memory`; the value, the
    // comparand and the old value are its element type.
    struct AtomicVariables
    {
        int previous = -1;
        int exchanged = -1;
    };

    AtomicVariables addAtomic(AtomicOperation operation,
                              MemorySpace memory,
                              int slot,
                              int index,
                              int value,
                              int compare = -1);
    int addAtomicAdd(int bufferSlot, int index, int value);
    int addAtomicLoad(int bufferSlot, int index);

    // The threadgroup pieces: where a thread sits inside its group and which
    // group it belongs to (components on the terms ThreadId sets - a 1D kernel
    // has only component 0), the implicit grid bound as a readable value, a
    // shared array with its subscript read and write, and the barrier that
    // orders them. Each id kind fixes the dispatch rank exactly as the global
    // ids do, so a kernel cannot mix a flat local id with a grid dispatch.
    int addLocalId();
    int addLocalPosition(int component);
    int addLocalPosition3(int component);
    int addGroupId();
    int addGroupPosition(int component);
    int addGroupPosition3(int component);

    // Their whole-vector forms, on the terms addThreadId2/addThreadId3 set.
    int addLocalId2();
    int addLocalId3();
    int addGroupId2();
    int addGroupId3();

    int addGridExtent(DispatchRank forRank, int component);
    int addSharedArray(ValueType elementType, int elements, bool atomic = false);
    int addSharedRead(int slot, int index);
    void addSharedStore(int slot, int index, int value);
    void addBarrier();

    // A fold over the threadgroup or over one SIMD group of it. Returns the
    // *variable* slot every thread's result lands in, which addVarRead then
    // reads - a statement like the atomic add, and one that counts as a barrier
    // for the bounds guard whichever scope it has, since the backends with no
    // wave intrinsic reach even the narrow one through threadgroup memory.
    int addGroupReduction(GroupReduction operation,
                          ValueType elementType,
                          int value,
                          ReductionScope scope = ReductionScope::Group);

    // The SIMD-group matrix statements. Each of the first two declares a
    // fragment and returns its slot, which is a numbering of its own: a
    // fragment is neither a variable nor a value, having no type any of the
    // three languages shares.
    int addSimdMatrixFill(int value);
    int addSimdMatrixLoad(SimdMatrixMemory memory,
                          int slot,
                          int index,
                          int stride,
                          SimdMatrixElement element = SimdMatrixElement::Float);
    void addSimdMatrixStore(
        int matrix, SimdMatrixMemory memory, int slot, int index, int stride);
    void addSimdMatrixMultiplyAdd(int accumulator, int left, int right);
    int addSimdGroupIndex();

    constexpr void setPosition(int node) { positionNode = node; }
    constexpr void setFragment(int node) { fragmentNode = node; }

    // The two fragment outputs beside the colour: the depth written in place
    // of the rasterized one (a Float), and the coverage mask ANDed with the
    // rasterizer's (a UInt). Either one, once set, turns the fragment's return
    // into a struct on Metal and HLSL.
    constexpr void setFragmentDepth(int node) { fragmentDepthNode = node; }
    constexpr void setSampleMask(int node) { sampleMaskNode = node; }

    // The alpha test: a third fragment-stage root, evaluated before the colour
    // is written. When the node's value falls below the threshold the fragment
    // is killed outright, writing neither colour nor depth.
    constexpr void setDiscard(int node, float threshold)
    {
        discardNode = node;
        discardValue = threshold;
    }

    const Expr& expr(int node) const;
    int nodeCount() const;
    constexpr const Vector<ValueType>& inputs() const { return inputTypes; }
    constexpr const Vector<StepRate>& inputStepRates() const { return inputRates; }
    constexpr const Vector<int>& inputBufferIndices() const { return inputSlots; }
    constexpr const Vector<VaryingSlot>& varyings() const { return varyingSlots; }
    constexpr const Vector<ValueType>& uniforms() const { return uniformTypes; }
    int textureCount() const;

    // How texture `slot` is to be sampled, as its shader declared it.
    TextureSampling textureSampling(int slot) const;

    // Whether the shader reads texture `slot` or writes it, which is what
    // decides the declaration each backend emits for it.
    TextureAccess textureAccess(int slot) const;

    // Whether texture `slot` is a 2D image or a cube - the other half of that
    // declaration, and the only other thing the emitter needs to print it.
    TextureKind textureKind(int slot) const;

    // The format a storage slot was declared with; a WritableTexture2D's is
    // RGBA32Float, which spells its float4 store in every dialect.
    TextureFormat textureFormat(int slot) const;

    constexpr int position() const { return positionNode; }
    constexpr int fragment() const { return fragmentNode; }
    constexpr int fragmentDepth() const { return fragmentDepthNode; }
    constexpr int sampleMask() const { return sampleMaskNode; }
    constexpr int discard() const { return discardNode; }
    constexpr float discardThreshold() const { return discardValue; }

    constexpr const Vector<BufferAccess>& storageBuffers() const
    {
        return storageSlots;
    }

    // What storage buffer `slot` holds, as its kernel declared it.
    ValueType storageElementType(int slot) const;

    // What shared array `slot` holds.
    ValueType sharedElementType(int slot) const;

    constexpr const Vector<ArrayConstant>& arrays() const { return arrayConstants; }
    constexpr const Vector<Store>& stores() const { return storeList; }
    constexpr const Vector<TextureStore>& textureStores() const
    {
        return textureStoreList;
    }
    constexpr const Vector<SharedArray>& sharedArrays() const
    {
        return sharedArrayList;
    }

    // The element types the kernel's reductions fold, one entry each, so the
    // emitter declares the scratch a reduction needs and no more. Both scopes
    // are in here, because a backend with no wave intrinsic stages the narrow
    // fold in the same array the wide one uses.
    constexpr const Vector<ValueType>& groupReductionTypes() const
    {
        return reductionTypes;
    }
    bool usesGroupReduction() const;

    // The subset folded over the *whole* group, which is the only scope Metal
    // needs an array for: there a SIMD-group fold is one instruction, and so is
    // a whole-group fold in a group no wider than a SIMD group.
    constexpr const Vector<ValueType>& wholeGroupReductionTypes() const
    {
        return wholeGroupTypes;
    }

    // Whether any reduction is collective over a SIMD group rather than over
    // the threadgroup - which puts the kernel under the same rule a SIMD-group
    // matrix is under, that the group has to be a whole number of SIMD groups.
    constexpr bool usesSimdReduction() const { return simdReductionUsed; }

    // How many bytes of threadgroup memory one group of this kernel takes: the
    // shared arrays it declared, plus the scratch the emitter adds behind them
    // for a reduction and for a SIMD-group matrix.
    //
    // The worst case of the three backends rather than this one's, since the
    // number is what a kernel is written against and a kernel is written once.
    // A vector element therefore counts as sixteen bytes, which is the stride
    // an std430 block and a DXBC groupshared array give it where MSL packs a
    // three-vector to twelve.
    //
    // Padding *between* arrays is not counted, so this is a bound on what the
    // declarations ask for rather than a byte-exact prediction of what the
    // backend will allocate.
    int threadgroupMemoryBytes() const;

    // How many 8x8 fragments the kernel declared, and whether it asked the
    // entry point for the SIMD-group vocabulary at all - a matrix statement or
    // a read of the SIMD group's index both do.
    int simdMatrixCount() const;

    bool usesSimdGroups() const;

    // What one fragment is made of, and whether any fragment at all is made of
    // a packed sixteen-bit element. The emitter takes the declared type and the
    // pointer reinterpret from the first; ComputeProgram::fitsPackedSimdMatrix
    // takes from the second the question it puts to the device.
    SimdMatrixElement simdMatrixElement(int matrix) const;

    bool usesPackedSimdMatrix(SimdMatrixElement element) const;

    // Which threadgroup pieces the kernel asked for, driving what the emitters
    // add to the entry signature - and, for the barrier, what they take away:
    // a kernel that barriers gets no early-return bounds guard, because a
    // barrier below a return some threads took is undefined on both backends.
    // Such a kernel bounds its own stores, typically against gridExtent.
    constexpr bool usesLocalId() const { return localIdUsed; }
    constexpr bool usesVertexId() const
    {
        return usesBuiltIn(ShaderBuiltIn::VertexId);
    }
    constexpr bool usesInstanceId() const
    {
        return usesBuiltIn(ShaderBuiltIn::InstanceId);
    }

    // Whether any node reads the built-in, which is what puts it in the entry
    // point's signature.
    constexpr bool usesBuiltIn(ShaderBuiltIn which) const
    {
        return (builtInsUsed & builtInBit(which)) != 0u;
    }
    constexpr bool usesGroupId() const { return groupIdUsed; }
    constexpr bool usesBarrier() const { return barrierUsed; }

    // Recording any store - to a buffer, to a texture, or an atomic add - is
    // what marks the graph as a kernel.
    //
    // The atomic case is easy to leave out and impossible to miss afterwards: a
    // kernel that only counts things writes nothing, so a graph judged by its
    // stores alone would emit a vertex/fragment pair for it and fail to compile
    // on a `gid` no render stage has.
    // A SIMD-group matrix is in the list for the same reason the atomic is: it
    // is a threadgroup facility with no render-stage spelling, so a kernel
    // whose only output is a fragment stored to a buffer is still a kernel.
    bool isCompute() const;

    constexpr DispatchRank dispatchRank() const { return rank; }

    // The group the kernel is dispatched in: what the author asked for, or the
    // stock shape for the rank recorded so far when they asked for nothing.
    constexpr void setThreadGroupShape(ThreadGroupShape shape)
    {
        groupShape = shape;
    }
    ThreadGroupShape threadGroupShape() const;

    // The body every statement recorded outside the vertex stage ends up in,
    // directly or inside a nested block. It runs before the fragment (or the
    // kernel's stores) is evaluated,
    // which is what makes a mutable local visible to the expression that reads
    // it afterwards.
    static constexpr int rootBlock = 0;

    // The vertex stage's own root block, or -1 while nothing was recorded in
    // it. Its statements run before the position and the varyings are written.
    constexpr int vertexBlock() const { return vertexRootBlock; }

    // Whether variable `slot` was declared in the vertex stage.
    bool isVertexVariable(int slot) const;

    const Statement& statement(int index) const;
    const Block& block(int index) const;
    int statementCount() const;
    int blockCount() const;
    constexpr const Vector<ValueType>& variables() const { return variableTypes; }
    bool hasStatements() const;

    // Whether a statement of this kind was recorded anywhere.
    bool hasStatementOf(StatementKind kind) const;

    // Whether any value, variable or storage buffer is of the f16 family -
    // what decides the GLSL extensions and the device's f16 query.
    bool usesHalf() const;

    // Whether a storage buffer holds f16 elements.
    bool usesHalfStorage() const;

    // Whether the fragment stage runs once per sample: it reads sampleIndex()
    // or a varying is Sample-interpolated. What the device's
    // supportsSampleRateShading query is asked about.
    bool usesSampleRateShading() const;

    // Whether a node's value is the same wherever in the body it is read: no
    // mutable state under it, save storage the kernel only reads.
    bool isPure(int node) const;

private:
    friend class FunctionInliner;

    static constexpr std::uint32_t builtInBit(ShaderBuiltIn which)
    {
        return 1u << static_cast<std::uint32_t>(which);
    }

    void checkFunctionBody(FunctionDeclaration& declaration, int id) const;
    void checkFunctionBlock(FunctionDeclaration& declaration,
                            int id,
                            int block,
                            Vector<char>& seen) const;
    void checkFunctionNode(FunctionDeclaration& declaration,
                           int id,
                           int node,
                           Vector<char>& seen) const;

    int add(Expr node);
    int addStatement(Statement newStatement);
    int addVariableSlot(ValueType type);
    int clampedIndex(int index, int size);
    BlockRole innermostJumpTarget() const;
    bool isInsideContinuing() const;
    bool areValidLabels(int selector, int cases) const;
    int addTextureSlot(TextureSampling sampling,
                       TextureAccess access,
                       TextureKind kind,
                       TextureFormat format = TextureFormat::RGBA32Float);

    // A fragment's slot, taken from a numbering of its own and remembering what
    // the fragment is made of.
    int declareSimdMatrix(SimdMatrixElement element);
    int addIndexNode(ExprKind kind, DispatchRank forRank, int component);

    int declareOverride(const OverrideDeclaration& declaration);
    bool matchesDeclaredStruct(const StructType& declared,
                               const StructType& type) const;
    void addStructFields(StructType& target,
                         const StructType& source,
                         int sourceId,
                         const std::string& prefix,
                         int baseOffset) const;
    bool structHoldsHalf(int structId) const;
    ValueType memberLeafType(int structId, int field) const;

    bool purityOf(const Expr& node) const;
    bool readsImmutableStorage(const Expr& node) const;
    int findShared(const Expr& node) const;

    struct Caches;

    class SharingCaches
    {
    public:
        SharingCaches();
        ~SharingCaches();
        SharingCaches(const SharingCaches& other);
        SharingCaches(SharingCaches&& other) noexcept;
        SharingCaches& operator=(const SharingCaches& other);
        SharingCaches& operator=(SharingCaches&& other) noexcept;

        const Caches* find() const;
        Caches& get();

    private:
        static std::unique_ptr<Caches> copyOf(const SharingCaches& other);

        std::unique_ptr<Caches> caches;
    };

    SharingCaches sharing;
    Vector<char> pureFlags; // parallel to nodes

    Vector<Expr> nodes;
    Vector<ValueType> inputTypes;
    Vector<StepRate> inputRates; // parallel to inputTypes
    Vector<int> inputSlots; // parallel to inputTypes; the buffer slot
    Vector<VaryingSlot> varyingSlots;
    Vector<ValueType> uniformTypes;
    Vector<std::string> uniformNames;
    Vector<std::string> storageNames;
    Vector<std::string> textureNames;
    Vector<BufferAccess> storageSlots;
    Vector<ValueType> storageElements; // parallel to storageSlots
    Vector<Store> storeList;
    Vector<TextureStore> textureStoreList;
    Vector<TextureSampling> textureSamplings;
    Vector<TextureAccess> textureAccesses; // parallel to textureSamplings
    Vector<TextureKind> textureKinds; // parallel to textureSamplings
    Vector<TextureFormat> textureFormats; // parallel to textureSamplings
    Vector<ArrayConstant> arrayConstants;
    Vector<SharedArray> sharedArrayList;
    Vector<ValueType> reductionTypes;
    Vector<ValueType> wholeGroupTypes;
    bool simdReductionUsed = false;
    Vector<SimdMatrixElement> simdMatrixElementList; // one entry per fragment
    bool simdGroupIndexUsed = false;
    bool localIdUsed = false;
    std::uint32_t builtInsUsed = 0u;
    bool groupIdUsed = false;
    bool barrierUsed = false;

    // Whether anything atomic was recorded. A kernel whose only output is a
    // counter has no store to be recognised by, so this is what tells the
    // emitter it is one - see isCompute.
    bool atomicUsed = false;

    Vector<ValueType> variableTypes;
    Vector<char> vertexVariables; // parallel to variableTypes
    Vector<int> variableSizes; // parallel to variableTypes; 0 = not an array
    Vector<int> variableStructs; // parallel to variableTypes; -1 = no struct
    Vector<int> storageStructs; // parallel to storageSlots; -1 = no struct
    Vector<StructType> structTypes;
    Vector<OverrideDeclaration> overrideList;
    Vector<FunctionDeclaration> functionList;
    Vector<int> variableFunctions; // parallel to variableTypes; -1 = a stage's
    int recordingFunction = -1;
    Vector<int> blocksOpenOutsideFunction;
    bool vertexStageOutsideFunction = false;
    Vector<Statement> statementList;
    Vector<Block> blocks; // blocks[rootBlock] is the shader's body
    Vector<CaseLabels> caseLabelList; // one entry per Case statement
    Vector<int> openBlocks; // innermost last; blocks[back()] takes new statements
    int vertexRootBlock = -1;
    bool recordingVertexStage = false;
    DispatchRank rank = DispatchRank::OneD;
    ThreadGroupShape groupShape;
    bool rankFixed = false;
    int positionNode = -1;
    int fragmentNode = -1;
    int fragmentDepthNode = -1;
    int sampleMaskNode = -1;
    int discardNode = -1;
    float discardValue = 0.0f;
};
} // namespace eacp::GPU
