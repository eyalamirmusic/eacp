#pragma once

#include "ShaderStruct.h"

#include <cassert>
#include <string>
#include <tuple>
#include <type_traits>

// WGSL's user functions. A C++ function called from define() is inlined into
// the graph: every call records its body again, so a helper used N times is
// emitted N times. A shader function is recorded once and emitted once per
// source as a real function with parameters and a result:
//
//     auto shade = function<Float(Float, Float2)>(
//         "shade",
//         [&](Float k, Float2 uv)
//         {
//             auto total = var(k);
//             auto early = [&] { returnEarly(total.get() * 2.f); };
//             ifThen(uv.x() > 0.5f, early);
//             return total.get() + uv.y();
//         });
//
//     auto a = shade(x, uv);
//     auto b = shade(a, uv * 2.f);
//
// The body runs once, when the function is declared, with handles for its
// parameters; what it returns is the result, and returnEarly(value) returns
// one from anywhere inside it - an ifThen, a loop, a switch. A function
// returning void returns nothing, and returnEarly() leaves it.
//
// Parameters and results are value handles - scalars, vectors and matrices of
// every family, f16 included - and shader structs (ShaderStruct.h), all by
// value; a parameter is read, never assigned, as in WGSL. No pointers.
//
// A body reads its parameters, its own var()s and private arrays, and what has
// module scope: literals, overrides, uniforms, buffers, textures and samplers,
// which MSL hands it as parameters of its own. It reads nothing a stage is
// handed - a built-in, a vertex input or varying, a thread index, shared
// memory, a constant array - nor a caller's variable: those are passed in as
// arguments. It may write buffers and textures and run atomics (a kernel's),
// and discard (a fragment's). WGSL checks a function against the stage calling
// it, and so does this: the vertex stage calls none that writes, runs an
// atomic, discards or takes derivatives, and a kernel none that discards or
// takes derivatives. A function calls functions declared before it, so none
// recurses, and none is declared inside another's body.
//
// A call of a function that returns a scalar, vector or matrix and neither
// touches writable storage nor discards is an expression, evaluated where it is
// read, in either render stage without vertexStage(). Any other call - void,
// returning a struct, writing or reading storage a kernel may write, or
// discarding - is a statement made where it is written, its result landing in
// a variable; in a render program it belongs to the stage open there, so a
// vertex one is called inside vertexStage().

namespace eacp::GPU
{
// What a function takes and returns.
template <typename T>
concept ShaderFunctionValue =
    ShaderStructLike<T> || requires { ValueTypeOf<T>::value; };

template <typename Signature>
struct ShaderFunction;

namespace detail
{
template <typename T>
int functionValueNode(ShaderGraph& graph, const T& value)
{
    if constexpr (ShaderStructLike<T>)
    {
        return structValueNode(graph, declareStruct<T>(graph), value);
    }
    else
    {
        assert(value.graph == &graph && value.node >= 0
               && "eacp: a function's argument or result is a value of the "
                  "shader declaring it");
        return value.node;
    }
}

template <typename T>
T makeFunctionParameter(ShaderGraph& graph)
{
    if constexpr (ShaderStructLike<T>)
    {
        auto structId = declareStruct<T>(graph);
        auto slot = graph.addParameter(ValueType::Struct, structId);
        auto makeLeaf = [&](int field)
        { return graph.addVarMemberRead(slot, field); };
        return makeStructValue<T>(graph, makeLeaf);
    }
    else
    {
        auto value = T {};
        value.graph = &graph;
        value.node = graph.addVarRead(graph.addParameter(ValueTypeOf<T>::value));
        return value;
    }
}

template <typename T>
T makeFunctionResult(ShaderGraph& graph, const ShaderGraph::FunctionResult& result)
{
    if constexpr (ShaderStructLike<T>)
    {
        auto makeLeaf = [&](int field)
        { return graph.addVarMemberRead(result.variable, field); };
        return makeStructValue<T>(graph, makeLeaf);
    }
    else
    {
        auto value = T {};
        value.graph = &graph;
        value.node = result.node;
        return value;
    }
}

template <typename Signature>
struct FunctionDefinition;

template <typename Result, typename... Args>
struct FunctionDefinition<Result(Args...)>
{
    static constexpr bool returnsValue = !std::is_void_v<Result>;

    static_assert((ShaderFunctionValue<Args> && ...),
                  "eacp: a function's parameters are value handles or shader "
                  "structs");

    static ValueType returnType()
    {
        if constexpr (!returnsValue)
            return ValueType::Bool;
        else if constexpr (ShaderStructLike<Result>)
            return ValueType::Struct;
        else
            return ValueTypeOf<Result>::value;
    }

    static int returnStruct(ShaderGraph& graph)
    {
        if constexpr (returnsValue && ShaderStructLike<Result>)
            return declareStruct<Result>(graph);
        else
            return -1;
    }

    template <typename Body>
    static ShaderFunction<Result(Args...)>
        define(ShaderGraph& graph, const std::string& name, Body& body)
    {
        auto structId = returnStruct(graph);
        auto id = graph.beginFunction(name, returnType(), structId, returnsValue);
        auto parameters =
            std::tuple<Args...> {makeFunctionParameter<Args>(graph)...};

        if constexpr (returnsValue)
        {
            static_assert(ShaderFunctionValue<Result>,
                          "eacp: a function returns a value handle, a shader "
                          "struct or void");

            const Result result = std::apply(body, parameters);
            graph.endFunction(functionValueNode(graph, result));
        }
        else
        {
            std::apply(body, parameters);
            graph.endFunction(-1);
        }

        return {&graph, id};
    }
};
} // namespace detail

// A function the shader declared (ShaderBuilder::function), called with one
// value per parameter of this graph.
template <typename Result, typename... Args>
struct ShaderFunction<Result(Args...)>
{
    Result operator()(const Args&... arguments) const
    {
        auto nodes = Vector<int> {};
        (nodes.add(detail::functionValueNode(*graph, arguments)), ...);

        auto result = graph->addFunctionCall(id, nodes);

        if constexpr (!std::is_void_v<Result>)
            return detail::makeFunctionResult<Result>(*graph, result);
    }

    ShaderGraph* graph = nullptr;
    int id = -1;
};
} // namespace eacp::GPU
