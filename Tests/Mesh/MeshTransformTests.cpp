#include "Common.h"

// The CPU matrix maths, which is the part of this module most able to be
// silently wrong.
//
// A wrong-but-plausible convention - a transpose, a row-major/column-major mix,
// a left-handed projection - still draws *something*, and something is what
// makes it hard to find. So every case here states an absolute answer rather
// than comparing two matrices against each other, and the projection cases pin
// the exact convention the EDSL's own perspective() builds, because the two are
// multiplied together in the shader.

using namespace nano;
using namespace eacp;
using namespace eacp::Mesh;
using namespace eacp::Mesh::Tests;

namespace
{
constexpr auto tight = 1.0e-5f;

Quat aroundZ(float radians)
{
    return {0.0f, 0.0f, std::sin(radians * 0.5f), std::cos(radians * 0.5f)};
}
} // namespace

auto tIdentityIsNeutral = test("MeshTransform/identityIsNeutral") = []
{
    auto translation = Mat4::translation({3.0f, -2.0f, 5.0f});
    auto product = translation * Mat4::identity();

    for (auto i = 0; i < 16; ++i)
        check(near(product.values[i], translation.values[i], tight));
};

// The multiply's operand order, stated as a point that moves. a * b means "do b,
// then a" - which is what makes parent * child the composition down a tree, and
// getting it backwards puts every child in its parent's frame reflected through
// the origin.
auto tCompositionAppliesRightmostFirst =
    test("MeshTransform/compositionAppliesRightmostFirst") = []
{
    auto parent = Mat4::translation({10.0f, 0.0f, 0.0f});
    auto child = Mat4::scaling({2.0f, 2.0f, 2.0f});

    auto composed = parent * child;
    auto moved = composed.transformPoint({1.0f, 0.0f, 0.0f});

    // Scaled to (2,0,0) and then translated: 12, not 22 (which is what
    // translating first and then scaling gives).
    check(near(moved, Vec3 {12.0f, 0.0f, 0.0f}, tight));
};

// glTF's own order for a node's TRS, which is not a free choice: a file writes
// translation, rotation and scale as three separate fields and the spec says
// how they combine. Scale first, then rotate, then translate.
auto tTrsAppliesScaleFirst = test("MeshTransform/trsAppliesScaleFirst") = []
{
    auto quarterTurn = aroundZ(3.14159265358979f * 0.5f);
    auto transform =
        Mat4::fromTRS({5.0f, 0.0f, 0.0f}, quarterTurn, {3.0f, 1.0f, 1.0f});

    // (1,0,0) scaled by 3 is (3,0,0); rotated a quarter turn about z is (0,3,0);
    // translated is (5,3,0). Rotating before scaling would give (5,1,0), which is
    // the mistake this pins.
    check(near(transform.transformPoint({1.0f, 0.0f, 0.0f}),
               Vec3 {5.0f, 3.0f, 0.0f},
               1.0e-4f));
};

// A direction is not a point: a translation must not move it. This is what the
// w of 0 does in the shader, and what transformDirection does here.
auto tDirectionIgnoresTranslation =
    test("MeshTransform/directionIgnoresTranslation") = []
{
    auto transform = Mat4::translation({100.0f, -50.0f, 7.0f});
    auto direction = transform.transformDirection({0.0f, 1.0f, 0.0f});

    check(near(direction, Vec3 {0.0f, 1.0f, 0.0f}, tight));
};

// The reason normalMatrix exists at all. Under a non-uniform scale, transforming
// a normal by the model matrix stops it being perpendicular to the surface - and
// the shading bends with it.
//
// The second half of this case is the important one: it asserts that the *wrong*
// way is actually wrong here. Without it, a normalMatrix that quietly returned
// the model matrix would pass.
auto tNormalMatrixStaysPerpendicular =
    test("MeshTransform/normalMatrixStaysPerpendicular") = []
{
    auto model = Mat4::scaling({3.0f, 1.0f, 1.0f});

    // A surface at 45 degrees in the xy plane: this tangent lies in it, and this
    // normal is perpendicular to it.
    auto tangent = Vec3 {1.0f, 1.0f, 0.0f};
    auto normal = Vec3 {1.0f, -1.0f, 0.0f};

    check(near(dot(tangent, normal), 0.0f, tight));

    auto movedTangent = model.transformDirection(tangent);
    auto correct = model.normalMatrix().transformDirection(normal);
    auto naive = model.transformDirection(normal);

    check(near(dot(movedTangent, correct), 0.0f, 1.0e-4f));
    check(!near(dot(movedTangent, naive), 0.0f, 0.1f));
};

// The flag the renderer picks a front face with. A mirrored instance reverses
// the winding of every triangle under it, and this determinant's sign is how
// that is detected without looking at the geometry.
auto tMirrorHasNegativeDeterminant =
    test("MeshTransform/mirrorHasNegativeDeterminant") = []
{
    check(Mat4::identity().linearDeterminant() > 0.0f);
    check(Mat4::scaling({2.0f, 3.0f, 4.0f}).linearDeterminant() > 0.0f);
    check(Mat4::scaling({-1.0f, 1.0f, 1.0f}).linearDeterminant() < 0.0f);

    // Two mirrors are a rotation, not a mirror - so a child of a mirrored parent
    // that is itself mirrored draws the right way round again.
    auto twice =
        Mat4::scaling({-1.0f, 1.0f, 1.0f}) * Mat4::scaling({1.0f, -1.0f, 1.0f});
    check(twice.linearDeterminant() > 0.0f);

    // A rotation never mirrors, however far round it goes.
    check(Mat4::rotation(aroundZ(2.5f)).linearDeterminant() > 0.0f);
};

// The projection convention, pinned absolutely. This has to be the same one
// ShaderBuilder::perspective builds or the two disagree in the shader, and depth
// mapped to the wrong range is a z-fight or an empty screen rather than an
// error.
auto tPerspectiveMapsDepthToZeroOne =
    test("MeshTransform/perspectiveMapsDepthToZeroOne") = []
{
    constexpr auto nearZ = 0.5f;
    constexpr auto farZ = 100.0f;

    auto projection = Mat4::perspective(1.0f, 1.0f, nearZ, farZ);

    // A right-handed camera looks down its own -z, so the near plane is at
    // z = -nearZ in view space, not +nearZ.
    auto projectPoint = [&](float z)
    {
        auto clipZ = projection.at(2, 2) * z + projection.at(3, 2);
        auto clipW = projection.at(2, 3) * z;
        return clipZ / clipW;
    };

    check(near(projectPoint(-nearZ), 0.0f, 1.0e-4f));
    check(near(projectPoint(-farZ), 1.0f, 1.0e-4f));

    // Halfway between the planes is *not* halfway in depth - perspective packs
    // precision towards the near plane. Asserting that keeps a linear
    // "projection" from passing the two cases above.
    auto middle = projectPoint(-(nearZ + farZ) * 0.5f);
    check(middle > 0.9f);
};

// The camera transform, which has the same handedness question in it.
auto tLookAtPutsTheTargetDownNegativeZ =
    test("MeshTransform/lookAtPutsTargetDownNegativeZ") = []
{
    auto view =
        Mat4::lookAt({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});

    // What the camera is looking at lands in front of it, which in a
    // right-handed view space is negative z.
    auto target = view.transformPoint({0.0f, 0.0f, 0.0f});
    check(near(target, Vec3 {0.0f, 0.0f, -10.0f}, 1.0e-4f));

    // And the camera's own position lands at the origin.
    auto eye = view.transformPoint({0.0f, 0.0f, 10.0f});
    check(near(eye, Vec3 {0.0f, 0.0f, 0.0f}, 1.0e-4f));

    // Up stays up rather than being flipped, which is the other way a lookAt
    // goes wrong and the one that renders a correct but upside-down picture.
    auto up = view.transformDirection({0.0f, 1.0f, 0.0f});
    check(near(up, Vec3 {0.0f, 1.0f, 0.0f}, 1.0e-4f));
};
