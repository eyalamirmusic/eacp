#pragma once

#include "../Common.h"

namespace eacp::GPUWidgets
{
// A 2D affine transform, applied to a point as
//
//     x' = a x + c y + tx
//     y' = b x + d y + ty
//
// which is the naming and the order SVG's matrix(a b c d e f) uses, and
// CGAffineTransform and D2D1::Matrix3x2F with it - so a document's six numbers
// land in the six fields without anyone having to work out a convention.
//
// Composition reads in application order: a.then(b) is a followed by b. That is
// the order a tree is walked in, parent before child, and the order a transform
// list is written in, so nothing has to be reversed at the call site.
struct AffineTransform
{
    static constexpr AffineTransform translation(float x, float y)
    {
        return {1.f, 0.f, 0.f, 1.f, x, y};
    }

    static constexpr AffineTransform scaling(float x, float y)
    {
        return {x, 0.f, 0.f, y, 0.f, 0.f};
    }

    static AffineTransform rotation(float radians);

    static AffineTransform skew(float radiansX, float radiansY);

    static AffineTransform rotationAbout(float radians,
                                         const Graphics::Point& centre);

    // This transform first, then `next`.
    constexpr AffineTransform then(const AffineTransform& next) const
    {
        return {next.a * a + next.c * b,
                next.b * a + next.d * b,
                next.a * c + next.c * d,
                next.b * c + next.d * d,
                next.a * tx + next.c * ty + next.tx,
                next.b * tx + next.d * ty + next.ty};
    }

    Graphics::Point apply(const Graphics::Point& point) const;

    // The upright rectangle round the four corners of `rect` once they have
    // been through this. Not the transform of a rectangle -- a turned one is
    // not a rectangle -- but what a caller placing a scissor, sizing a texture
    // or asking what was touched actually needs.
    Graphics::Rect apply(const Graphics::Rect& rect) const;

    // How much this magnifies a length, as one number: the square root of the
    // area scale. Exact for a rotation or a uniform scale, and the usual
    // compromise for anything else - a stroke width has to become a single
    // number whatever the matrix did to the two axes, and this is the number SVG
    // itself picks for that.
    float getScaleFactor() const;

    constexpr float getDeterminant() const { return a * d - b * c; }

    // The transform that undoes this one, or the identity when there is none --
    // which is a matrix that collapsed the plane onto a line or a point, and has
    // no inverse to give. A caller that needs to know asks getDeterminant.
    AffineTransform inverted() const;

    constexpr bool isIdentity() const
    {
        return a == 1.f && b == 0.f && c == 0.f && d == 1.f && tx == 0.f
               && ty == 0.f;
    }

    // Exactly, since what asks is a cache deciding whether two things were
    // placed by the same matrix -- and two matrices composed the same way out of
    // the same numbers are bit-identical, while two that merely look alike are a
    // different placement and deserve their own entry.
    constexpr bool operator==(const AffineTransform& other) const
    {
        return a == other.a && b == other.b && c == other.c && d == other.d
               && tx == other.tx && ty == other.ty;
    }

    float a = 1.f;
    float b = 0.f;
    float c = 0.f;
    float d = 1.f;
    float tx = 0.f;
    float ty = 0.f;
};
} // namespace eacp::GPUWidgets
