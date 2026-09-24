#include "Interpreter.h"

#include <climits>
#include <cmath>

// Cases the GPU leaves undefined are defined here as plan.md D7 spells them.

namespace eacp::GPU::CpuCompute
{
namespace
{
using Lanes::toFloat;
using Lanes::toSigned;
using Lanes::toWord;

float builtinRsqrt(float x)
{
    return 1.f / std::sqrt(x);
}

float builtinFract(float x)
{
    auto result = x - std::floor(x);
    return result >= 1.f ? 0x1.fffffep-1f : result;
}

float builtinSign(float x)
{
    if (x > 0.f)
        return 1.f;

    if (x < 0.f)
        return -1.f;

    return x == x ? x : 0.f;
}

float builtinClamp(float x, float low, float high)
{
    return std::fmin(std::fmax(x, low), high);
}

float builtinSmoothstep(float edge0, float edge1, float x)
{
    auto t = builtinClamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

std::int32_t builtinIntFromFloat(float x)
{
    if (x != x)
        return 0;

    if (x >= 2147483648.f)
        return INT_MAX;

    if (x <= -2147483648.f)
        return INT_MIN;

    return static_cast<std::int32_t>(x);
}

std::uint32_t builtinUIntFromFloat(float x)
{
    if (!(x > 0.f))
        return 0u;

    if (x >= 4294967296.f)
        return UINT_MAX;

    return static_cast<std::uint32_t>(x);
}

template <typename Function>
void builtinMap(const Context& context, const Plan::Node& node, Function function)
{
    for (auto component = 0; component < node.components; ++component)
        Lanes::mapFloats(context.lanes(node, component),
                         context.operand(node, 0, component),
                         context.stride,
                         function);
}

template <typename Function>
void builtinZip(const Context& context, const Plan::Node& node, Function function)
{
    for (auto component = 0; component < node.components; ++component)
        Lanes::zipFloats(context.lanes(node, component),
                         context.operand(node, 0, component),
                         context.operand(node, 1, component),
                         context.stride,
                         function);
}

template <typename Function>
void builtinZip3(const Context& context, const Plan::Node& node, Function function)
{
    for (auto component = 0; component < node.components; ++component)
        Lanes::zipFloats3(context.lanes(node, component),
                          context.operand(node, 0, component),
                          context.operand(node, 1, component),
                          context.operand(node, 2, component),
                          context.stride,
                          function);
}

template <typename Function>
void builtinWords(const Context& context, const Plan::Node& node, Function function)
{
    for (auto component = 0; component < node.components; ++component)
        Lanes::mapWords(context.lanes(node, component),
                        context.operand(node, 0, component),
                        context.stride,
                        function);
}

template <typename Function>
void builtinZipWords(const Context& context,
                     const Plan::Node& node,
                     Function function)
{
    for (auto component = 0; component < node.components; ++component)
        Lanes::zipWords(context.lanes(node, component),
                        context.operand(node, 0, component),
                        context.operand(node, 1, component),
                        context.stride,
                        function);
}

template <typename Function>
void builtinZipSigned(const Context& context,
                      const Plan::Node& node,
                      Function function)
{
    for (auto component = 0; component < node.components; ++component)
        Lanes::zipSigned(context.lanes(node, component),
                         context.operand(node, 0, component),
                         context.operand(node, 1, component),
                         context.stride,
                         function);
}

void builtinUnaryMath(const Context& context, const Plan::Node& node)
{
    switch ((MathFunction) node.sub)
    {
        case MathFunction::Sin:
            builtinMap(context, node, [](float x) { return std::sin(x); });
            return;
        case MathFunction::Cos:
            builtinMap(context, node, [](float x) { return std::cos(x); });
            return;
        case MathFunction::Tan:
            builtinMap(context, node, [](float x) { return std::tan(x); });
            return;
        case MathFunction::Asin:
            builtinMap(context, node, [](float x) { return std::asin(x); });
            return;
        case MathFunction::Acos:
            builtinMap(context, node, [](float x) { return std::acos(x); });
            return;
        case MathFunction::Atan:
            builtinMap(context, node, [](float x) { return std::atan(x); });
            return;
        case MathFunction::Sinh:
            builtinMap(context, node, [](float x) { return std::sinh(x); });
            return;
        case MathFunction::Cosh:
            builtinMap(context, node, [](float x) { return std::cosh(x); });
            return;
        case MathFunction::Tanh:
            builtinMap(context, node, [](float x) { return std::tanh(x); });
            return;
        case MathFunction::Exp:
            builtinMap(context, node, [](float x) { return std::exp(x); });
            return;
        case MathFunction::Exp2:
            builtinMap(context, node, [](float x) { return std::exp2(x); });
            return;
        case MathFunction::Log:
            builtinMap(context, node, [](float x) { return std::log(x); });
            return;
        case MathFunction::Log2:
            builtinMap(context, node, [](float x) { return std::log2(x); });
            return;
        case MathFunction::Log10:
            builtinMap(context, node, [](float x) { return std::log10(x); });
            return;
        case MathFunction::Sqrt:
            builtinMap(context, node, [](float x) { return std::sqrt(x); });
            return;
        case MathFunction::Rsqrt:
            builtinMap(context, node, builtinRsqrt);
            return;
        case MathFunction::Floor:
            builtinMap(context, node, [](float x) { return std::floor(x); });
            return;
        case MathFunction::Ceil:
            builtinMap(context, node, [](float x) { return std::ceil(x); });
            return;
        case MathFunction::Trunc:
            builtinMap(context, node, [](float x) { return std::trunc(x); });
            return;
        case MathFunction::Round:
            builtinMap(context, node, [](float x) { return std::round(x); });
            return;
        case MathFunction::Fract:
            builtinMap(context, node, builtinFract);
            return;
        case MathFunction::Sign:
            builtinMap(context, node, builtinSign);
            return;
        case MathFunction::Abs:
            builtinMap(context, node, [](float x) { return std::fabs(x); });
            return;
    }
}

struct VectorLanes
{
    const Word* component[4] = {};
};

VectorLanes builtinVector(const Context& context,
                          const Plan::Node& node,
                          int which,
                          int width)
{
    auto vector = VectorLanes {};

    for (auto component = 0; component < width; ++component)
        vector.component[component] = context.operand(node, which, component);

    return vector;
}

struct OutputLanes
{
    Word* component[4] = {};
};

OutputLanes builtinOutput(const Context& context, const Plan::Node& node, int width)
{
    auto output = OutputLanes {};

    for (auto component = 0; component < width; ++component)
        output.component[component] = context.lanes(node, component);

    return output;
}

float builtinDotAt(const VectorLanes& a, const VectorLanes& b, int width, int lane)
{
    auto sum = toFloat(a.component[0][lane]) * toFloat(b.component[0][lane]);

    for (auto component = 1; component < width; ++component)
        sum = sum
              + toFloat(a.component[component][lane])
                    * toFloat(b.component[component][lane]);

    return sum;
}

float builtinDistanceAt(const VectorLanes& a,
                        const VectorLanes& b,
                        int width,
                        int lane)
{
    auto sum = 0.f;

    for (auto component = 0; component < width; ++component)
    {
        auto difference = toFloat(a.component[component][lane])
                          - toFloat(b.component[component][lane]);
        sum =
            component == 0 ? difference * difference : sum + difference * difference;
    }

    return std::sqrt(sum);
}

void builtinGeometric(const Context& context, const Plan::Node& node)
{
    auto width = static_cast<int>(node.order);
    auto stride = context.stride;
    auto a = builtinVector(context, node, 0, width);

    switch (node.op)
    {
        case Op::Dot:
        {
            auto b = builtinVector(context, node, 1, width);
            auto* out = context.lanes(node, 0);

            for (auto lane = 0; lane < stride; ++lane)
                out[lane] = toWord(builtinDotAt(a, b, width, lane));

            return;
        }

        case Op::Length:
        {
            auto* out = context.lanes(node, 0);

            for (auto lane = 0; lane < stride; ++lane)
                out[lane] = toWord(std::sqrt(builtinDotAt(a, a, width, lane)));

            return;
        }

        case Op::Distance:
        {
            auto b = builtinVector(context, node, 1, width);
            auto* out = context.lanes(node, 0);

            for (auto lane = 0; lane < stride; ++lane)
                out[lane] = toWord(builtinDistanceAt(a, b, width, lane));

            return;
        }

        case Op::Normalize:
        {
            auto out = builtinOutput(context, node, width);

            for (auto lane = 0; lane < stride; ++lane)
            {
                auto scale = builtinRsqrt(builtinDotAt(a, a, width, lane));

                for (auto component = 0; component < width; ++component)
                    out.component[component][lane] =
                        toWord(toFloat(a.component[component][lane]) * scale);
            }

            return;
        }

        case Op::Cross:
        {
            auto b = builtinVector(context, node, 1, 3);
            auto out = builtinOutput(context, node, 3);

            for (auto lane = 0; lane < stride; ++lane)
            {
                auto ax = toFloat(a.component[0][lane]);
                auto ay = toFloat(a.component[1][lane]);
                auto az = toFloat(a.component[2][lane]);
                auto bx = toFloat(b.component[0][lane]);
                auto by = toFloat(b.component[1][lane]);
                auto bz = toFloat(b.component[2][lane]);

                out.component[0][lane] = toWord(ay * bz - az * by);
                out.component[1][lane] = toWord(az * bx - ax * bz);
                out.component[2][lane] = toWord(ax * by - ay * bx);
            }

            return;
        }

        case Op::Reflect:
        {
            auto normal = builtinVector(context, node, 1, width);
            auto out = builtinOutput(context, node, width);

            for (auto lane = 0; lane < stride; ++lane)
            {
                auto twice = 2.f * builtinDotAt(normal, a, width, lane);

                for (auto component = 0; component < width; ++component)
                    out.component[component][lane] =
                        toWord(toFloat(a.component[component][lane])
                               - twice * toFloat(normal.component[component][lane]));
            }

            return;
        }

        case Op::Refract:
        {
            auto normal = builtinVector(context, node, 1, width);
            const auto* etas = context.operand(node, 2, 0);
            auto out = builtinOutput(context, node, width);

            for (auto lane = 0; lane < stride; ++lane)
            {
                auto eta = toFloat(etas[lane]);
                auto cosine = builtinDotAt(normal, a, width, lane);
                auto k = 1.f - eta * eta * (1.f - cosine * cosine);

                for (auto component = 0; component < width; ++component)
                {
                    auto incident = toFloat(a.component[component][lane]);
                    auto n = toFloat(normal.component[component][lane]);
                    auto refracted =
                        eta * incident - (eta * cosine + std::sqrt(k)) * n;

                    out.component[component][lane] =
                        toWord(k < 0.f ? 0.f : refracted);
                }
            }

            return;
        }

        case Op::FaceForward:
        {
            auto incident = builtinVector(context, node, 1, width);
            auto reference = builtinVector(context, node, 2, width);
            auto out = builtinOutput(context, node, width);

            for (auto lane = 0; lane < stride; ++lane)
            {
                auto facing = builtinDotAt(reference, incident, width, lane) < 0.f;

                for (auto component = 0; component < width; ++component)
                {
                    auto n = toFloat(a.component[component][lane]);
                    out.component[component][lane] = toWord(facing ? n : -n);
                }
            }

            return;
        }

        default:
            return;
    }
}

void builtinTranspose(const Context& context, const Plan::Node& node)
{
    auto order = static_cast<int>(node.order);

    for (auto column = 0; column < order; ++column)
        for (auto row = 0; row < order; ++row)
            Lanes::copy(context.lanes(node, column * order + row),
                        context.operand(node, 0, row * order + column),
                        context.stride);
}

struct MatrixLanes
{
    const Word* element[16] = {};
    int order = 0;

    float at(int column, int row, int lane) const
    {
        return toFloat(element[column * order + row][lane]);
    }
};

float builtinDeterminant2(float a, float b, float c, float d)
{
    return a * d - c * b;
}

float builtinDeterminant3(const float m[9])
{
    auto first = m[0] * builtinDeterminant2(m[4], m[5], m[7], m[8]);
    auto second = m[3] * builtinDeterminant2(m[1], m[2], m[7], m[8]);
    auto third = m[6] * builtinDeterminant2(m[1], m[2], m[4], m[5]);
    return first - second + third;
}

float builtinDeterminantAt(const MatrixLanes& matrix, int lane)
{
    auto order = matrix.order;

    if (order == 2)
        return builtinDeterminant2(matrix.at(0, 0, lane),
                                   matrix.at(0, 1, lane),
                                   matrix.at(1, 0, lane),
                                   matrix.at(1, 1, lane));

    if (order == 3)
    {
        float m[9];

        for (auto index = 0; index < 9; ++index)
            m[index] = matrix.at(index / 3, index % 3, lane);

        return builtinDeterminant3(m);
    }

    auto sum = 0.f;

    for (auto column = 0; column < 4; ++column)
    {
        float minor[9];
        auto written = 0;

        for (auto other = 0; other < 4; ++other)
        {
            if (other == column)
                continue;

            for (auto row = 1; row < 4; ++row)
                minor[written++] = matrix.at(other, row, lane);
        }

        auto term = matrix.at(column, 0, lane) * builtinDeterminant3(minor);
        sum = column == 0 ? term : (column % 2 == 0 ? sum + term : sum - term);
    }

    return sum;
}

void builtinDeterminant(const Context& context, const Plan::Node& node)
{
    auto matrix = MatrixLanes {};
    matrix.order = node.order;

    for (auto index = 0; index < matrix.order * matrix.order; ++index)
        matrix.element[index] = context.operand(node, 0, index);

    auto* out = context.lanes(node, 0);

    for (auto lane = 0; lane < context.stride; ++lane)
        out[lane] = toWord(builtinDeterminantAt(matrix, lane));
}

void builtinReduceMask(const Context& context, const Plan::Node& node, bool all)
{
    auto* out = context.lanes(node, 0);
    Lanes::copy(out, context.operand(node, 0, 0), context.stride);

    for (auto component = 1; component < static_cast<int>(node.order); ++component)
    {
        const auto* mask = context.operand(node, 0, component);

        for (auto lane = 0; lane < context.stride; ++lane)
            out[lane] = all ? out[lane] & mask[lane] : out[lane] | mask[lane];
    }
}
} // namespace

void evaluateCall(const Context& context, const Plan::Node& node)
{
    switch (node.op)
    {
        case Op::UnaryMath:
            builtinUnaryMath(context, node);
            return;

        case Op::CopyBits:
            for (auto component = 0; component < node.components; ++component)
                Lanes::copy(context.lanes(node, component),
                            context.operand(node, 0, component),
                            context.stride);
            return;

        case Op::AbsS:
            builtinWords(context,
                         node,
                         [](Word a) { return toSigned(a) < 0 ? Word {0} - a : a; });
            return;

        case Op::MinF:
            builtinZip(
                context, node, [](float a, float b) { return std::fmin(a, b); });
            return;

        case Op::MaxF:
            builtinZip(
                context, node, [](float a, float b) { return std::fmax(a, b); });
            return;

        case Op::MinU:
            builtinZipWords(
                context, node, [](Word a, Word b) { return a < b ? a : b; });
            return;

        case Op::MaxU:
            builtinZipWords(
                context, node, [](Word a, Word b) { return a > b ? a : b; });
            return;

        case Op::MinS:
            builtinZipSigned(context,
                             node,
                             [](std::int32_t a, std::int32_t b)
                             { return a < b ? a : b; });
            return;

        case Op::MaxS:
            builtinZipSigned(context,
                             node,
                             [](std::int32_t a, std::int32_t b)
                             { return a > b ? a : b; });
            return;

        case Op::Pow:
            builtinZip(
                context, node, [](float a, float b) { return std::pow(a, b); });
            return;

        case Op::Atan2:
            builtinZip(
                context, node, [](float y, float x) { return std::atan2(y, x); });
            return;

        case Op::Step:
            builtinZip(context,
                       node,
                       [](float edge, float x) { return x < edge ? 0.f : 1.f; });
            return;

        case Op::Clamp:
            builtinZip3(context, node, builtinClamp);
            return;

        case Op::Mix:
            builtinZip3(context,
                        node,
                        [](float a, float b, float t) { return a + (b - a) * t; });
            return;

        case Op::Smoothstep:
            builtinZip3(context, node, builtinSmoothstep);
            return;

        case Op::Dot:
        case Op::Length:
        case Op::Distance:
        case Op::Cross:
        case Op::Normalize:
        case Op::Reflect:
        case Op::Refract:
        case Op::FaceForward:
            builtinGeometric(context, node);
            return;

        case Op::Transpose:
            builtinTranspose(context, node);
            return;

        case Op::Determinant:
            builtinDeterminant(context, node);
            return;

        case Op::All:
            builtinReduceMask(context, node, true);
            return;

        case Op::Any:
            builtinReduceMask(context, node, false);
            return;

        case Op::FloatFromU:
            builtinWords(
                context, node, [](Word a) { return toWord(static_cast<float>(a)); });
            return;

        case Op::FloatFromS:
            builtinWords(context,
                         node,
                         [](Word a)
                         { return toWord(static_cast<float>(toSigned(a))); });
            return;

        case Op::FloatFromMask:
            builtinWords(
                context, node, [](Word a) { return toWord(a != 0 ? 1.f : 0.f); });
            return;

        case Op::IntFromF:
            builtinWords(
                context,
                node,
                [](Word a)
                { return Lanes::fromSigned(builtinIntFromFloat(toFloat(a))); });
            return;

        case Op::UIntFromF:
            builtinWords(context,
                         node,
                         [](Word a) { return builtinUIntFromFloat(toFloat(a)); });
            return;

        case Op::IntFromMask:
            builtinWords(context, node, [](Word a) { return a & 1u; });
            return;

        default:
            return;
    }
}
} // namespace eacp::GPU::CpuCompute
