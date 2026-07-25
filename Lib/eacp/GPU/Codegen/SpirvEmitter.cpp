#include "ShaderEmitter.h"

#include "ShaderGraph.h"
#include "UniformLayout.h"

#include <cstring>
#include <vector>

// SPIR-V generation for the Vulkan backend. The graph is walked once per stage
// and each node becomes an SSA result id, which is why no local-naming pass is
// needed here the way the text emitter needs one: SPIR-V is already SSA, so
// memoising a node's id gives the sharing that emitLocals() spells out by hand.

namespace eacp::GPU
{
namespace
{
constexpr std::uint32_t magic = 0x07230203;
constexpr std::uint32_t version = 0x00010000; // SPIR-V 1.0: every Vulkan takes it

enum Op : std::uint32_t
{
    OpName = 5,
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampledImage = 27,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpVectorShuffle = 79,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpSampledImage = 86,
    OpImageSampleImplicitLod = 87,
    OpFNegate = 127,
    OpFAdd = 129,
    OpFSub = 131,
    OpFMul = 133,
    OpFDiv = 136,
    OpMatrixTimesVector = 145,
    OpDot = 148,
    OpFOrdLessThan = 184,
    OpSelectionMerge = 247,
    OpLabel = 248,
    OpBranchConditional = 250,
    OpKill = 252,
    OpReturn = 253
};

enum Decoration : std::uint32_t
{
    DecorationBlock = 2,
    DecorationColMajor = 5,
    DecorationMatrixStride = 7,
    DecorationBuiltIn = 11,
    DecorationLocation = 30,
    DecorationBinding = 33,
    DecorationDescriptorSet = 34,
    DecorationOffset = 35
};

enum StorageClass : std::uint32_t
{
    StorageUniformConstant = 0,
    StorageInput = 1,
    StorageOutput = 3,
    StorageFunction = 7,
    StoragePushConstant = 9
};

// GLSL.std.450 extended instruction numbers.
enum Glsl : std::uint32_t
{
    GlslFAbs = 4,
    GlslFSign = 6,
    GlslFloor = 8,
    GlslCeil = 9,
    GlslFract = 10,
    GlslSin = 13,
    GlslCos = 14,
    GlslTan = 15,
    GlslPow = 26,
    GlslExp = 27,
    GlslLog = 28,
    GlslSqrt = 31,
    GlslInverseSqrt = 32,
    GlslFMin = 37,
    GlslFMax = 40,
    GlslFClamp = 43,
    GlslFMix = 46,
    GlslStep = 48,
    GlslSmoothStep = 49,
    GlslLength = 66,
    GlslCross = 68,
    GlslNormalize = 69,
    GlslReflect = 71
};

void writeInstruction(Vector<std::uint32_t>& out,
                      std::uint32_t opcode,
                      const std::vector<std::uint32_t>& operands)
{
    auto words = static_cast<std::uint32_t>(operands.size() + 1);
    out.add((words << 16) | opcode);

    for (auto operand: operands)
        out.add(operand);
}

// SPIR-V literal strings are UTF-8 packed four bytes to a word, null terminated,
// and padded out to the next word boundary.
std::vector<std::uint32_t> literalString(const std::string& text)
{
    auto words = std::vector<std::uint32_t> {};
    auto word = std::uint32_t {};
    auto byte = 0;

    for (auto character: text)
    {
        word |= static_cast<std::uint32_t>(static_cast<unsigned char>(character))
                << (8 * byte);

        if (++byte == 4)
        {
            words.push_back(word);
            word = 0;
            byte = 0;
        }
    }

    words.push_back(word); // the terminating null lands in the current word
    return words;
}

void append(std::vector<std::uint32_t>& into, const std::vector<std::uint32_t>& from)
{
    into.insert(into.end(), from.begin(), from.end());
}

struct Module
{
    std::uint32_t id() { return nextId++; }

    void decorate(std::uint32_t target,
                  std::uint32_t decoration,
                  std::uint32_t operand)
    {
        writeInstruction(decorations, OpDecorate, {target, decoration, operand});
    }

    // Block and ColMajor carry no literal of their own; passing one would
    // lengthen the instruction and desynchronise everything after it.
    void decorate(std::uint32_t target, std::uint32_t decoration)
    {
        writeInstruction(decorations, OpDecorate, {target, decoration});
    }

    void memberDecorate(std::uint32_t target,
                        std::uint32_t member,
                        std::uint32_t decoration,
                        std::uint32_t operand)
    {
        writeInstruction(
            decorations, OpMemberDecorate, {target, member, decoration, operand});
    }

    void memberDecorate(std::uint32_t target,
                        std::uint32_t member,
                        std::uint32_t decoration)
    {
        writeInstruction(
            decorations, OpMemberDecorate, {target, member, decoration});
    }

    std::uint32_t typeVoid()
    {
        if (voidType == 0)
        {
            voidType = id();
            writeInstruction(declarations, OpTypeVoid, {voidType});
        }

        return voidType;
    }

    std::uint32_t typeBool()
    {
        if (boolType == 0)
        {
            boolType = id();
            writeInstruction(declarations, OpTypeBool, {boolType});
        }

        return boolType;
    }

    std::uint32_t typeFloat()
    {
        if (floatType == 0)
        {
            floatType = id();
            writeInstruction(declarations, OpTypeFloat, {floatType, 32});
        }

        return floatType;
    }

    std::uint32_t typeUint()
    {
        if (uintType == 0)
        {
            uintType = id();
            writeInstruction(declarations, OpTypeInt, {uintType, 32, 0});
        }

        return uintType;
    }

    std::uint32_t typeVector(int components)
    {
        auto& slot = vectorTypes[components];

        if (slot == 0)
        {
            slot = id();
            writeInstruction(
                declarations,
                OpTypeVector,
                {slot, typeFloat(), static_cast<std::uint32_t>(components)});
        }

        return slot;
    }

    std::uint32_t typeMatrix()
    {
        if (matrixType == 0)
        {
            matrixType = id();
            writeInstruction(
                declarations, OpTypeMatrix, {matrixType, typeVector(4), 4});
        }

        return matrixType;
    }

    std::uint32_t typeOf(ValueType type)
    {
        switch (type)
        {
            case ValueType::Float:
                return typeFloat();
            case ValueType::Float2:
                return typeVector(2);
            case ValueType::Float3:
                return typeVector(3);
            case ValueType::Float4:
                return typeVector(4);
            case ValueType::Float4x4:
                return typeMatrix();
            case ValueType::UInt:
                return typeUint();
        }

        return typeFloat();
    }

    std::uint32_t typePointer(std::uint32_t storage, std::uint32_t pointee)
    {
        for (const auto& entry: pointerTypes)
            if (entry.storage == storage && entry.pointee == pointee)
                return entry.result;

        auto result = id();
        writeInstruction(declarations, OpTypePointer, {result, storage, pointee});
        pointerTypes.push_back({storage, pointee, result});

        return result;
    }

    std::uint32_t constantFloat(float value)
    {
        auto bits = std::uint32_t {};
        std::memcpy(&bits, &value, sizeof(bits));

        for (const auto& entry: floatConstants)
            if (entry.first == bits)
                return entry.second;

        auto result = id();
        writeInstruction(declarations, OpConstant, {typeFloat(), result, bits});
        floatConstants.push_back({bits, result});

        return result;
    }

    std::uint32_t constantUint(std::uint32_t value)
    {
        for (const auto& entry: uintConstants)
            if (entry.first == value)
                return entry.second;

        auto result = id();
        writeInstruction(declarations, OpConstant, {typeUint(), result, value});
        uintConstants.push_back({value, result});

        return result;
    }

    std::uint32_t variable(std::uint32_t pointerType, std::uint32_t storage)
    {
        auto result = id();
        writeInstruction(declarations, OpVariable, {pointerType, result, storage});
        return result;
    }

    Vector<std::uint32_t> finish()
    {
        auto words = Vector<std::uint32_t> {};
        words.add(magic);
        words.add(version);
        words.add(0); // generator: unregistered
        words.add(nextId); // id bound
        words.add(0); // reserved

        for (auto* section: {&capabilities,
                             &extensions,
                             &entryPoints,
                             &executionModes,
                             &names,
                             &decorations,
                             &declarations,
                             &code})
            for (auto word: *section)
                words.add(word);

        return words;
    }

    std::uint32_t nextId = 1;
    std::uint32_t glslSet = 0;

    Vector<std::uint32_t> capabilities;
    Vector<std::uint32_t> extensions;
    Vector<std::uint32_t> entryPoints;
    Vector<std::uint32_t> executionModes;
    Vector<std::uint32_t> names;
    Vector<std::uint32_t> decorations;
    Vector<std::uint32_t> declarations;
    Vector<std::uint32_t> code;

private:
    struct PointerType
    {
        std::uint32_t storage = 0;
        std::uint32_t pointee = 0;
        std::uint32_t result = 0;
    };

    std::uint32_t voidType = 0;
    std::uint32_t boolType = 0;
    std::uint32_t floatType = 0;
    std::uint32_t uintType = 0;
    std::uint32_t matrixType = 0;
    std::uint32_t vectorTypes[5] = {};
    std::vector<PointerType> pointerTypes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> floatConstants;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> uintConstants;
};

std::uint32_t glslOpcode(const std::string& name)
{
    if (name == "abs")
        return GlslFAbs;
    if (name == "sign")
        return GlslFSign;
    if (name == "floor")
        return GlslFloor;
    if (name == "ceil")
        return GlslCeil;
    if (name == "fract")
        return GlslFract;
    if (name == "sin")
        return GlslSin;
    if (name == "cos")
        return GlslCos;
    if (name == "tan")
        return GlslTan;
    if (name == "pow")
        return GlslPow;
    if (name == "exp")
        return GlslExp;
    if (name == "log")
        return GlslLog;
    if (name == "sqrt")
        return GlslSqrt;
    if (name == "rsqrt")
        return GlslInverseSqrt;
    if (name == "min")
        return GlslFMin;
    if (name == "max")
        return GlslFMax;
    if (name == "clamp")
        return GlslFClamp;
    if (name == "mix")
        return GlslFMix;
    if (name == "step")
        return GlslStep;
    if (name == "smoothstep")
        return GlslSmoothStep;
    if (name == "length")
        return GlslLength;
    if (name == "cross")
        return GlslCross;
    if (name == "normalize")
        return GlslNormalize;
    if (name == "reflect")
        return GlslReflect;

    return 0;
}

int swizzleIndex(char component)
{
    switch (component)
    {
        case 'x':
        case 'r':
            return 0;
        case 'y':
        case 'g':
            return 1;
        case 'z':
        case 'b':
            return 2;
        default:
            return 3;
    }
}

// Emits one stage's expression tree. A node's id is computed once and reused,
// which is the SSA equivalent of the text emitter's tN locals.
struct StageEmitter
{
    StageEmitter(const ShaderGraph& graphToUse, Module& moduleToUse, int nodeCount)
        : graph(graphToUse)
        , module(moduleToUse)
        , ids(static_cast<std::size_t>(nodeCount), 0)
    {
    }

    std::uint32_t emit(int node)
    {
        if (node < 0)
            return 0;

        if (ids[static_cast<std::size_t>(node)] != 0)
            return ids[static_cast<std::size_t>(node)];

        auto result = build(node);
        ids[static_cast<std::size_t>(node)] = result;

        return result;
    }

    // Widens a scalar to the result vector so a component-wise operation sees
    // two operands of one type: the EDSL freely writes `vec * 0.5`, which
    // SPIR-V will not accept without the splat.
    std::uint32_t splat(std::uint32_t value, ValueType from, ValueType to)
    {
        if (from == to || componentCount(to) == 1 || componentCount(from) != 1)
            return value;

        auto operands = std::vector<std::uint32_t> {module.typeOf(to), module.id()};
        auto result = operands[1];

        for (auto i = 0; i < componentCount(to); ++i)
            operands.push_back(value);

        writeInstruction(module.code, OpCompositeConstruct, operands);

        return result;
    }

    std::uint32_t load(std::uint32_t pointer, std::uint32_t type)
    {
        auto result = module.id();
        writeInstruction(module.code, OpLoad, {type, result, pointer});
        return result;
    }

    std::uint32_t build(int node)
    {
        const auto& expr = graph.expr(node);
        auto type = module.typeOf(expr.type);

        switch (expr.kind)
        {
            case ExprKind::Input:
                return load(inputs[static_cast<std::size_t>(expr.index)], type);

            case ExprKind::Varying:
                return load(varyings[static_cast<std::size_t>(expr.index)], type);

            case ExprKind::Uniform:
            {
                auto pointerType = module.typePointer(StoragePushConstant, type);
                auto chain = module.id();
                writeInstruction(
                    module.code,
                    OpAccessChain,
                    {pointerType,
                     chain,
                     uniformBlock,
                     module.constantUint(static_cast<std::uint32_t>(expr.index))});

                return load(chain, type);
            }

            case ExprKind::Constant:
                if (expr.type == ValueType::UInt)
                    return module.constantUint(
                        static_cast<std::uint32_t>(expr.index));

                return module.constantFloat(expr.value);

            case ExprKind::Construct:
            {
                auto operands = std::vector<std::uint32_t> {type, module.id()};
                auto result = operands[1];

                for (auto argument: expr.args)
                    operands.push_back(emit(argument));

                writeInstruction(module.code, OpCompositeConstruct, operands);
                return result;
            }

            case ExprKind::Swizzle:
            {
                auto source = emit(expr.args[0]);
                auto result = module.id();

                if (expr.text.size() == 1)
                {
                    writeInstruction(
                        module.code,
                        OpCompositeExtract,
                        {type,
                         result,
                         source,
                         static_cast<std::uint32_t>(swizzleIndex(expr.text[0]))});

                    return result;
                }

                auto operands =
                    std::vector<std::uint32_t> {type, result, source, source};

                for (auto component: expr.text)
                    operands.push_back(
                        static_cast<std::uint32_t>(swizzleIndex(component)));

                writeInstruction(module.code, OpVectorShuffle, operands);
                return result;
            }

            case ExprKind::Call:
            {
                auto result = module.id();

                // dot is a core instruction rather than a GLSL.std.450 one.
                if (expr.text == "dot")
                {
                    writeInstruction(
                        module.code,
                        OpDot,
                        {type, result, emit(expr.args[0]), emit(expr.args[1])});
                    return result;
                }

                auto operands = std::vector<std::uint32_t> {
                    type, result, module.glslSet, glslOpcode(expr.text)};

                for (auto argument: expr.args)
                    operands.push_back(emit(argument));

                writeInstruction(module.code, OpExtInst, operands);
                return result;
            }

            case ExprKind::Unary:
            {
                auto result = module.id();
                writeInstruction(
                    module.code, OpFNegate, {type, result, emit(expr.args[0])});
                return result;
            }

            case ExprKind::Binary:
            {
                auto lhs = splat(
                    emit(expr.args[0]), graph.expr(expr.args[0]).type, expr.type);
                auto rhs = splat(
                    emit(expr.args[1]), graph.expr(expr.args[1]).type, expr.type);

                auto opcode = std::uint32_t {OpFAdd};

                if (expr.op == '-')
                    opcode = OpFSub;
                else if (expr.op == '*')
                    opcode = OpFMul;
                else if (expr.op == '/')
                    opcode = OpFDiv;

                auto result = module.id();
                writeInstruction(module.code, opcode, {type, result, lhs, rhs});
                return result;
            }

            case ExprKind::Mul:
            {
                // Matrix times vector. SPIR-V matrices are column-major and
                // OpCompositeConstruct fills them from columns, which is the
                // MSL convention the graph is written against, so no transpose
                // is needed the way HLSL needs one.
                auto result = module.id();
                writeInstruction(
                    module.code,
                    OpMatrixTimesVector,
                    {type, result, emit(expr.args[0]), emit(expr.args[1])});
                return result;
            }

            case ExprKind::Sample:
            {
                auto slot = static_cast<std::size_t>(expr.index);
                auto combined = load(textures[slot], sampledImageType);
                auto result = module.id();

                writeInstruction(
                    module.code,
                    OpImageSampleImplicitLod,
                    {module.typeVector(4), result, combined, emit(expr.args[0])});

                return result;
            }

            case ExprKind::ThreadId:
            case ExprKind::BufferRead:
                // Compute is not part of the Vulkan backend yet; a graph with
                // these nodes never reaches here (emitSpirv refuses it).
                return 0;
        }

        return 0;
    }

    const ShaderGraph& graph;
    Module& module;
    std::vector<std::uint32_t> ids;
    std::vector<std::uint32_t> inputs;
    std::vector<std::uint32_t> varyings;
    std::vector<std::uint32_t> textures;
    std::uint32_t uniformBlock = 0;
    std::uint32_t sampledImageType = 0;
};
} // namespace

Vector<std::uint32_t> emitSpirv(const ShaderGraph& graph)
{
    if (graph.isCompute())
        return {};

    auto module = Module {};

    writeInstruction(module.capabilities, OpCapability, {1}); // Shader

    module.glslSet = module.id();
    {
        auto operands = std::vector<std::uint32_t> {module.glslSet};
        append(operands, literalString("GLSL.std.450"));
        writeInstruction(module.extensions, OpExtInstImport, operands);
    }

    writeInstruction(module.extensions, OpMemoryModel, {0, 1}); // Logical, GLSL450

    auto vertexFunction = module.id();
    auto fragmentFunction = module.id();

    auto voidType = module.typeVoid();
    auto functionType = module.id();
    writeInstruction(module.declarations, OpTypeFunction, {functionType, voidType});

    auto vec4 = module.typeVector(4);

    // The push-constant block carries every uniform<>() call, offset exactly as
    // UniformLayout packs them on the CPU, so one packed upload feeds all three
    // backends. Bound to both stages, matching setVertexBytes/setFragmentBytes.
    auto uniformTypes = graph.uniforms();
    auto hasUniforms = uniformTypes.size() > 0;
    auto uniformVariable = std::uint32_t {};

    if (hasUniforms)
    {
        auto structType = module.id();
        auto members = std::vector<std::uint32_t> {structType};

        for (auto type: uniformTypes)
            members.push_back(module.typeOf(type));

        writeInstruction(module.declarations, OpTypeStruct, members);
        module.decorate(structType, DecorationBlock);

        auto offsets = uniformOffsets(uniformTypes);

        for (auto i = 0; i < uniformTypes.size(); ++i)
        {
            module.memberDecorate(structType,
                                  static_cast<std::uint32_t>(i),
                                  DecorationOffset,
                                  static_cast<std::uint32_t>(offsets[i]));

            if (uniformTypes[i] == ValueType::Float4x4)
            {
                module.memberDecorate(
                    structType, static_cast<std::uint32_t>(i), DecorationColMajor);
                module.memberDecorate(structType,
                                      static_cast<std::uint32_t>(i),
                                      DecorationMatrixStride,
                                      16);
            }
        }

        uniformVariable =
            module.variable(module.typePointer(StoragePushConstant, structType),
                            StoragePushConstant);
    }

    auto inputVariables = std::vector<std::uint32_t> {};

    for (auto i = 0; i < graph.inputs().size(); ++i)
    {
        auto type = module.typeOf(graph.inputs()[i]);
        auto variable =
            module.variable(module.typePointer(StorageInput, type), StorageInput);
        module.decorate(variable, DecorationLocation, static_cast<std::uint32_t>(i));
        inputVariables.push_back(variable);
    }

    auto positionVariable =
        module.variable(module.typePointer(StorageOutput, vec4), StorageOutput);
    module.decorate(positionVariable, DecorationBuiltIn, 0); // Position

    auto varyingOutputs = std::vector<std::uint32_t> {};
    auto varyingInputs = std::vector<std::uint32_t> {};

    for (auto i = 0; i < graph.varyings().size(); ++i)
    {
        auto type = module.typeOf(graph.varyings()[i].type);
        auto location = static_cast<std::uint32_t>(i);

        auto output =
            module.variable(module.typePointer(StorageOutput, type), StorageOutput);
        module.decorate(output, DecorationLocation, location);
        varyingOutputs.push_back(output);

        auto input =
            module.variable(module.typePointer(StorageInput, type), StorageInput);
        module.decorate(input, DecorationLocation, location);
        varyingInputs.push_back(input);
    }

    auto colorVariable =
        module.variable(module.typePointer(StorageOutput, vec4), StorageOutput);
    module.decorate(colorVariable, DecorationLocation, 0);

    // Combined image samplers with immutable samplers in the set layout, so the
    // sampling a shader declared is fixed where D3D12 fixes it in the root
    // signature: binding = slot * configurations + configuration, the same
    // arithmetic the HLSL emitter uses to pick a sampler register.
    auto sampledImageType = std::uint32_t {};
    auto textureVariables = std::vector<std::uint32_t> {};

    if (graph.textureCount() > 0)
    {
        // Sampled type, Dim2D, not depth, not arrayed, not multisampled, used
        // with a sampler, format Unknown.
        auto imageType = module.id();
        writeInstruction(module.declarations,
                         OpTypeImage,
                         {imageType, module.typeFloat(), 1, 0, 0, 0, 1, 0});

        sampledImageType = module.id();
        writeInstruction(
            module.declarations, OpTypeSampledImage, {sampledImageType, imageType});

        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto variable = module.variable(
                module.typePointer(StorageUniformConstant, sampledImageType),
                StorageUniformConstant);

            module.decorate(variable, DecorationDescriptorSet, 0);
            module.decorate(variable,
                            DecorationBinding,
                            static_cast<std::uint32_t>(
                                i * samplingConfigurations
                                + samplingIndex(graph.textureSampling(i))));

            textureVariables.push_back(variable);
        }
    }

    {
        auto operands = std::vector<std::uint32_t> {0, vertexFunction}; // Vertex
        append(operands, literalString("vertexMain"));

        for (auto variable: inputVariables)
            operands.push_back(variable);

        operands.push_back(positionVariable);

        for (auto variable: varyingOutputs)
            operands.push_back(variable);

        writeInstruction(module.entryPoints, OpEntryPoint, operands);
    }

    {
        auto operands = std::vector<std::uint32_t> {4, fragmentFunction}; // Fragment
        append(operands, literalString("fragmentMain"));

        for (auto variable: varyingInputs)
            operands.push_back(variable);

        operands.push_back(colorVariable);

        writeInstruction(module.entryPoints, OpEntryPoint, operands);
    }

    writeInstruction(module.executionModes, OpExecutionMode, {fragmentFunction, 7});

    {
        writeInstruction(
            module.code, OpFunction, {voidType, vertexFunction, 0, functionType});
        writeInstruction(module.code, OpLabel, {module.id()});

        auto stage = StageEmitter {graph, module, graph.nodeCount()};
        stage.inputs = inputVariables;
        stage.uniformBlock = uniformVariable;
        stage.sampledImageType = sampledImageType;

        writeInstruction(
            module.code, OpStore, {positionVariable, stage.emit(graph.position())});

        for (auto i = 0; i < graph.varyings().size(); ++i)
            writeInstruction(module.code,
                             OpStore,
                             {varyingOutputs[static_cast<std::size_t>(i)],
                              stage.emit(graph.varyings()[i].sourceNode)});

        writeInstruction(module.code, OpReturn, {});
        writeInstruction(module.code, OpFunctionEnd, {});
    }

    {
        writeInstruction(
            module.code, OpFunction, {voidType, fragmentFunction, 0, functionType});
        writeInstruction(module.code, OpLabel, {module.id()});

        auto stage = StageEmitter {graph, module, graph.nodeCount()};
        stage.varyings = varyingInputs;
        stage.textures = textureVariables;
        stage.uniformBlock = uniformVariable;
        stage.sampledImageType = sampledImageType;

        if (graph.discard() >= 0)
        {
            auto value = stage.emit(graph.discard());
            auto condition = module.id();

            writeInstruction(module.code,
                             OpFOrdLessThan,
                             {module.typeBool(),
                              condition,
                              value,
                              module.constantFloat(graph.discardThreshold())});

            auto killLabel = module.id();
            auto mergeLabel = module.id();

            writeInstruction(module.code, OpSelectionMerge, {mergeLabel, 0});
            writeInstruction(module.code,
                             OpBranchConditional,
                             {condition, killLabel, mergeLabel});
            writeInstruction(module.code, OpLabel, {killLabel});
            writeInstruction(module.code, OpKill, {});
            writeInstruction(module.code, OpLabel, {mergeLabel});
        }

        writeInstruction(
            module.code, OpStore, {colorVariable, stage.emit(graph.fragment())});

        writeInstruction(module.code, OpReturn, {});
        writeInstruction(module.code, OpFunctionEnd, {});
    }

    return module.finish();
}
} // namespace eacp::GPU
