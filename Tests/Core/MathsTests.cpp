#include "Common.h"

#include <limits>

using namespace nano;
using namespace eacp::Maths;

auto tVectorsDefaultToZero = test("Maths/vectorsDefaultToZero") = []
{
    check(Vec2 {} == Vec2 {0.f, 0.f});
    check(Vec3 {} == Vec3 {0.f, 0.f, 0.f});
    check(Vec4 {} == Vec4 {0.f, 0.f, 0.f, 0.f});
};

// The whole point of the types: a Vec3 is three floats and nothing else, so it
// can be a vertex field or the CPU half of a Float3 uniform without repacking.
auto tVectorsArePackedFloats = test("Maths/vectorsArePackedFloats") = []
{
    static_assert(sizeof(Vec2) == 2 * sizeof(float));
    static_assert(sizeof(Vec3) == 3 * sizeof(float));
    static_assert(sizeof(Vec4) == 4 * sizeof(float));
    static_assert(sizeof(Mat4) == 16 * sizeof(float));

    static_assert(std::is_trivially_copyable_v<Vec3>);
    static_assert(std::is_trivially_copyable_v<Mat4>);

    auto v = Vec3 {1.f, 2.f, 3.f};
    auto* asFloats = reinterpret_cast<const float*>(&v);

    check(asFloats[0] == 1.f);
    check(asFloats[1] == 2.f);
    check(asFloats[2] == 3.f);
};

auto tVectorArithmetic = test("Maths/vectorArithmetic") = []
{
    auto a = Vec3 {1.f, 2.f, 3.f};
    auto b = Vec3 {4.f, 5.f, 6.f};

    check(a + b == Vec3 {5.f, 7.f, 9.f});
    check(b - a == Vec3 {3.f, 3.f, 3.f});
    check(a * 2.f == Vec3 {2.f, 4.f, 6.f});
    check(2.f * a == Vec3 {2.f, 4.f, 6.f});
    check(b / 2.f == Vec3 {2.f, 2.5f, 3.f});
    check(-a == Vec3 {-1.f, -2.f, -3.f});
    check(a * b == Vec3 {4.f, 10.f, 18.f});
};

auto tVectorCompoundAssignment = test("Maths/vectorCompoundAssignment") = []
{
    auto v = Vec2 {1.f, 2.f};

    v += {3.f, 4.f};
    check(v == Vec2 {4.f, 6.f});

    v -= {1.f, 1.f};
    check(v == Vec2 {3.f, 5.f});

    v *= 2.f;
    check(v == Vec2 {6.f, 10.f});

    v /= 2.f;
    check(v == Vec2 {3.f, 5.f});
};

auto tDotAndCross = test("Maths/dotAndCross") = []
{
    check(dot(Vec3 {1.f, 2.f, 3.f}, Vec3 {4.f, 5.f, 6.f}) == 32.f);
    check(dot(Vec2 {1.f, 0.f}, Vec2 {0.f, 1.f}) == 0.f);

    auto x = Vec3 {1.f, 0.f, 0.f};
    auto y = Vec3 {0.f, 1.f, 0.f};

    check(cross(x, y) == Vec3 {0.f, 0.f, 1.f});
    check(cross(y, x) == Vec3 {0.f, 0.f, -1.f});
};

auto tLengthAndDistance = test("Maths/lengthAndDistance") = []
{
    check(length(Vec2 {3.f, 4.f}) == 5.f);
    check(lengthSquared(Vec2 {3.f, 4.f}) == 25.f);
    check(distance(Vec3 {1.f, 0.f, 0.f}, Vec3 {4.f, 4.f, 0.f}) == 5.f);
};

auto tNormalize = test("Maths/normalize") = []
{
    check(normalize(Vec3 {0.f, 5.f, 0.f}) == Vec3 {0.f, 1.f, 0.f});
    check(nearlyEqual(length(normalize(Vec3 {1.f, 2.f, 3.f})), 1.f));
};

// Without the guard this divides by (near enough) zero and every component
// comes back NaN, which then spreads through whatever the vector feeds.
auto tNormalizeOfZeroIsZero = test("Maths/normalizeOfZeroIsZero") = []
{
    check(normalize(Vec3 {}) == Vec3 {});
    check(normalize(Vec3 {1.e-9f, 0.f, 0.f}) == Vec3 {});
};

auto tMinMaxAndLerp = test("Maths/minMaxAndLerp") = []
{
    auto a = Vec3 {1.f, 5.f, 3.f};
    auto b = Vec3 {4.f, 2.f, 3.f};

    check(min(a, b) == Vec3 {1.f, 2.f, 3.f});
    check(max(a, b) == Vec3 {4.f, 5.f, 3.f});

    check(lerp(Vec2 {0.f, 0.f}, Vec2 {10.f, 20.f}, 0.5f) == Vec2 {5.f, 10.f});
    check(lerp(Vec2 {1.f, 2.f}, Vec2 {3.f, 4.f}, 0.f) == Vec2 {1.f, 2.f});
    check(lerp(Vec2 {1.f, 2.f}, Vec2 {3.f, 4.f}, 1.f) == Vec2 {3.f, 4.f});
};

auto tSwizzles = test("Maths/swizzles") = []
{
    check(Vec4 {1.f, 2.f, 3.f, 4.f}.xyz() == Vec3 {1.f, 2.f, 3.f});
    check(Vec4 {1.f, 2.f, 3.f, 4.f}.xy() == Vec2 {1.f, 2.f});
    check(Vec3 {1.f, 2.f, 3.f}.xy() == Vec2 {1.f, 2.f});
};

auto tVectorArithmeticIsConstexpr = test("Maths/vectorArithmeticIsConstexpr") = []
{
    constexpr auto sum = Vec3 {1.f, 2.f, 3.f} + Vec3 {1.f, 1.f, 1.f};
    static_assert(sum == Vec3 {2.f, 3.f, 4.f});
    static_assert(dot(Vec3 {1.f, 0.f, 0.f}, Vec3 {1.f, 0.f, 0.f}) == 1.f);

    check(true);
};

auto tMatrixDefaultsToIdentity = test("Maths/matrixDefaultsToIdentity") = []
{
    auto identity = Mat4 {};

    for (auto column = 0; column < 4; ++column)
        for (auto row = 0; row < 4; ++row)
            check(identity.at(column, row) == (column == row ? 1.f : 0.f));

    auto point = Vec3 {3.f, -2.f, 7.f};
    check(transformPoint(identity, point) == point);
};

// Column-major: the translation lives in the last four floats, which is what
// lets one of these upload into a Float4x4 uniform with nothing transposed.
auto tMatrixIsColumnMajor = test("Maths/matrixIsColumnMajor") = []
{
    auto move = Mat4::translation({1.f, 2.f, 3.f});

    check(move.values[12] == 1.f);
    check(move.values[13] == 2.f);
    check(move.values[14] == 3.f);
    check(move.values[15] == 1.f);
    check(move.at(3, 0) == 1.f);
    check(move.column(3) == Vec4 {1.f, 2.f, 3.f, 1.f});
};

auto tTranslationAndScale = test("Maths/translationAndScale") = []
{
    auto moved = transformPoint(Mat4::translation({1.f, 2.f, 3.f}), {1.f, 1.f, 1.f});
    check(moved == Vec3 {2.f, 3.f, 4.f});

    auto scaled = transformPoint(Mat4::scale({2.f, 3.f, 4.f}), {1.f, 1.f, 1.f});
    check(scaled == Vec3 {2.f, 3.f, 4.f});

    check(transformPoint(Mat4::scale(2.f), {1.f, 2.f, 3.f}) == Vec3 {2.f, 4.f, 6.f});
};

// A direction ignores the translation; a point does not.
auto tTransformDirectionIgnoresTranslation =
    test("Maths/transformDirectionIgnoresTranslation") = []
{
    auto move = Mat4::translation({5.f, 5.f, 5.f});
    check(transformDirection(move, {1.f, 0.f, 0.f}) == Vec3 {1.f, 0.f, 0.f});
};

auto tRotations = test("Maths/rotations") = []
{
    auto quarter = halfPi;

    auto aboutX = transformPoint(Mat4::rotationX(quarter), {0.f, 1.f, 0.f});
    check(nearlyEqual(aboutX, {0.f, 0.f, 1.f}));

    auto aboutY = transformPoint(Mat4::rotationY(quarter), {0.f, 0.f, 1.f});
    check(nearlyEqual(aboutY, {1.f, 0.f, 0.f}));

    auto aboutZ = transformPoint(Mat4::rotationZ(quarter), {1.f, 0.f, 0.f});
    check(nearlyEqual(aboutZ, {0.f, 1.f, 0.f}));
};

// Right-to-left, the order the shader EDSL and both native APIs read a product
// in: the rightmost matrix touches the point first.
auto tMultiplicationOrder = test("Maths/multiplicationOrder") = []
{
    auto scale = Mat4::scale(2.f);
    auto move = Mat4::translation({10.f, 0.f, 0.f});

    check(transformPoint(move * scale, {1.f, 0.f, 0.f}) == Vec3 {12.f, 0.f, 0.f});
    check(transformPoint(scale * move, {1.f, 0.f, 0.f}) == Vec3 {22.f, 0.f, 0.f});
};

auto tMultiplyingByIdentity = test("Maths/multiplyingByIdentity") = []
{
    auto transform = Mat4::translation({1.f, 2.f, 3.f}) * Mat4::rotationY(0.7f);

    check(nearlyEqual(transform * Mat4 {}, transform));
    check(nearlyEqual(Mat4 {} * transform, transform));
};

auto tTransposed = test("Maths/transposed") = []
{
    auto move = Mat4::translation({1.f, 2.f, 3.f});
    auto transposed = move.transposed();

    check(transposed.at(0, 3) == 1.f);
    check(transposed.at(1, 3) == 2.f);
    check(transposed.at(2, 3) == 3.f);
    check(nearlyEqual(transposed.transposed(), move));
};

auto tInverted = test("Maths/inverted") = []
{
    auto transform = Mat4::translation({3.f, -1.f, 4.f}) * Mat4::rotationZ(0.9f)
                     * Mat4::scale({2.f, 0.5f, 3.f});

    check(nearlyEqual(transform * transform.inverted(), Mat4 {}));

    auto point = Vec3 {1.5f, -2.f, 0.25f};
    auto roundTrip =
        transformPoint(transform.inverted(), transformPoint(transform, point));
    check(nearlyEqual(roundTrip, point));
};

// No inverse exists, so the identity comes back rather than a block of NaNs
// that would spread silently through everything downstream.
auto tInvertedSingularIsIdentity = test("Maths/invertedSingularIsIdentity") = []
{ check(Mat4::scale({1.f, 0.f, 1.f}).inverted() == Mat4 {}); };

// Right-handed with a [0, 1] depth range: the camera looks down -z, the near
// plane lands on 0 and the far plane on 1. Matching ShaderProgram::perspective
// is what lets a CPU-built matrix and a shader-built one agree.
auto tPerspectiveDepthRange = test("Maths/perspectiveDepthRange") = []
{
    auto projection = Mat4::perspective(1.f, radians(60.f), 1.f, 100.f);

    auto onNear = projection * Vec4 {0.f, 0.f, -1.f, 1.f};
    auto onFar = projection * Vec4 {0.f, 0.f, -100.f, 1.f};

    check(nearlyEqual(onNear.z / onNear.w, 0.f));
    check(nearlyEqual(onFar.z / onFar.w, 1.f));

    // w is the view-space distance, which is what the divide by w needs it to be.
    check(nearlyEqual(onNear.w, 1.f));
    check(nearlyEqual(onFar.w, 100.f));
};

auto tPerspectiveAspect = test("Maths/perspectiveAspect") = []
{
    auto wide = Mat4::perspective(2.f, radians(90.f), 0.1f, 10.f);

    // A 90 degree vertical field: the top of the frustum is at y = -z.
    auto top = wide * Vec4 {0.f, 5.f, -5.f, 1.f};
    check(nearlyEqual(top.y / top.w, 1.f));

    // Twice as wide, so x has to reach twice as far to hit the same edge.
    auto side = wide * Vec4 {10.f, 0.f, -5.f, 1.f};
    check(nearlyEqual(side.x / side.w, 1.f));
};

auto tOrthographicDepthRange = test("Maths/orthographicDepthRange") = []
{
    auto projection = Mat4::orthographic(-2.f, 2.f, -1.f, 1.f, 1.f, 5.f);

    check(nearlyEqual(transformPoint(projection, {0.f, 0.f, -1.f}).z, 0.f));
    check(nearlyEqual(transformPoint(projection, {0.f, 0.f, -5.f}).z, 1.f));

    check(nearlyEqual(transformPoint(projection, {2.f, 1.f, -1.f}).xy(),
                      Vec2 {1.f, 1.f}));
    check(nearlyEqual(transformPoint(projection, {-2.f, -1.f, -1.f}).xy(),
                      Vec2 {-1.f, -1.f}));
};

// The view matrix puts the camera at the origin looking down -z, so the point
// it was aimed at lands straight ahead of it.
auto tLookAt = test("Maths/lookAt") = []
{
    auto view = Mat4::lookAt({0.f, 0.f, 5.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});

    check(nearlyEqual(transformPoint(view, {0.f, 0.f, 5.f}), {0.f, 0.f, 0.f}));
    check(nearlyEqual(transformPoint(view, {0.f, 0.f, 0.f}), {0.f, 0.f, -5.f}));
    check(nearlyEqual(transformPoint(view, {1.f, 2.f, 5.f}), {1.f, 2.f, 0.f}));
};

auto tLookAtFromAnyAxis = test("Maths/lookAtFromAnyAxis") = []
{
    auto eye = Vec3 {4.f, 0.f, 0.f};
    auto view = Mat4::lookAt(eye, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});

    check(nearlyEqual(transformPoint(view, eye), {0.f, 0.f, 0.f}));
    check(nearlyEqual(transformPoint(view, {0.f, 0.f, 0.f}), {0.f, 0.f, -4.f}));
    check(nearlyEqual(transformPoint(view, eye + Vec3 {0.f, 1.f, 0.f}),
                      {0.f, 1.f, 0.f}));
};

// `up` is a hint, not a basis vector: the component of it along the view
// direction is removed, so a tilted one still gives an orthonormal frame.
auto tLookAtOrthonormalisesUp = test("Maths/lookAtOrthonormalisesUp") = []
{
    auto view = Mat4::lookAt({0.f, 0.f, 3.f}, {0.f, 0.f, 0.f}, {0.2f, 4.f, 0.f});

    auto right = Vec3 {view.at(0, 0), view.at(1, 0), view.at(2, 0)};
    auto up = Vec3 {view.at(0, 1), view.at(1, 1), view.at(2, 1)};
    auto back = Vec3 {view.at(0, 2), view.at(1, 2), view.at(2, 2)};

    check(nearlyEqual(length(right), 1.f));
    check(nearlyEqual(length(up), 1.f));
    check(nearlyEqual(length(back), 1.f));
    check(nearlyEqual(dot(right, up), 0.f));
    check(nearlyEqual(dot(right, back), 0.f));
    check(nearlyEqual(dot(up, back), 0.f));
};

// The comparison every other case here is written against, so it is worth
// pinning: slack by default, an exact answer when the slack is taken away, and
// never true for a NaN.
auto tNearlyEqual = test("Maths/nearlyEqual") = []
{
    check(nearlyEqual(1.f, 1.f + 1.e-7f));
    check(!nearlyEqual(1.f, 1.001f));
    check(!nearlyEqual(1.f, 1.f + 1.e-7f, 0.f));
    check(nearlyEqual(1.f, 1.5f, 0.5f));

    // Doubles compare as doubles: a slack a float could not resolve still works.
    check(nearlyEqual(1.0, 1.0 + 1.e-12, 1.e-11));
    check(!nearlyEqual(1.0, 1.0 + 1.e-12, 1.e-13));

    auto nan = std::numeric_limits<float>::quiet_NaN();
    check(!nearlyEqual(nan, nan));
    check(!nearlyEqual(nan, 0.f));

    check(nearlyEqual(Vec3 {1.f, 2.f, 3.f}, Vec3 {1.f, 2.f, 3.f + 1.e-7f}));
    check(!nearlyEqual(Vec3 {1.f, 2.f, 3.f}, Vec3 {1.f, 2.f, 3.01f}));
    check(nearlyEqual(Mat4 {}, Mat4::scale(1.f + 1.e-7f)));
    check(!nearlyEqual(Mat4 {}, Mat4::scale(1.01f)));
};

auto tRadiansAndDegrees = test("Maths/radiansAndDegrees") = []
{
    check(nearlyEqual(radians(180.f), pi));
    check(nearlyEqual(degrees(pi), 180.f));
    check(nearlyEqual(degrees(radians(37.5f)), 37.5f));
};

// Whatever precision the caller wrote the literal in is the precision they get
// back, and a double gets a double's worth of pi rather than a float's.
auto tAngleConversionsKeepTheirPrecision =
    test("Maths/angleConversionsKeepTheirPrecision") = []
{
    static_assert(std::is_same_v<decltype(radians(5.f)), float>);
    static_assert(std::is_same_v<decltype(radians(5.0)), double>);
    static_assert(std::is_same_v<decltype(degrees(5.f)), float>);
    static_assert(std::is_same_v<decltype(degrees(5.0)), double>);

    constexpr auto exactHalfPi = 1.5707963267948966;
    check(nearlyEqual(radians(90.0), exactHalfPi, 1.e-15));
    check(nearlyEqual(degrees(radians(37.5)), 37.5, 1.e-13));
};
