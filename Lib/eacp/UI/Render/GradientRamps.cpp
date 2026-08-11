#include "GradientRamps.h"

#include "ContentHash.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
std::uint64_t hashStops(const Vector<GradientStop>& stops)
{
    auto hash = ContentHash {};

    for (const auto& stop: stops)
    {
        hash.mix(stop.position);
        hash.mix(stop.color.r);
        hash.mix(stop.color.g);
        hash.mix(stop.color.b);
        hash.mix(stop.color.a);
    }

    return hash.get();
}

bool sameStops(const Vector<GradientStop>& a, const Vector<GradientStop>& b)
{
    if (a.size() != b.size())
        return false;

    for (auto i = 0; i < a.size(); ++i)
    {
        if (a[i].position != b[i].position || a[i].color.r != b[i].color.r
            || a[i].color.g != b[i].color.g || a[i].color.b != b[i].color.b
            || a[i].color.a != b[i].color.a)
            return false;
    }

    return true;
}

// `stops` must already be sorted by position.
Color colourAt(const Vector<GradientStop>& stops, float t)
{
    if (t <= stops.front().position)
        return stops.front().color;

    if (t >= stops.back().position)
        return stops.back().color;

    for (auto i = 1; i < stops.size(); ++i)
    {
        const auto& before = stops[i - 1];
        const auto& after = stops[i];

        if (t > after.position)
            continue;

        auto span = after.position - before.position;

        // A hard edge, where the spec says the later stop wins.
        if (span <= 0.f)
            return after.color;

        auto amount = (t - before.position) / span;

        return {before.color.r + (after.color.r - before.color.r) * amount,
                before.color.g + (after.color.g - before.color.g) * amount,
                before.color.b + (after.color.b - before.color.b) * amount,
                before.color.a + (after.color.a - before.color.a) * amount};
    }

    return stops.back().color;
}

std::uint8_t toByte(float value)
{
    return (std::uint8_t) std::lround(std::clamp(value, 0.f, 1.f) * 255.f);
}

// Stable, two stops at one position being a hard edge the document ordered.
Vector<GradientStop> sortedAndClamped(const Vector<GradientStop>& stops)
{
    auto sorted = stops;

    for (auto& stop: sorted)
        stop.position = std::clamp(stop.position, 0.f, 1.f);

    std::stable_sort(sorted.begin(),
                     sorted.end(),
                     [](const GradientStop& a, const GradientStop& b)
                     { return a.position < b.position; });

    return sorted;
}
} // namespace

GradientRamps::GradientRamps()
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = rampWidth;
    descriptor.height = maxRows;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;

    texture.emplace(GPU::Device::shared(), descriptor, nullptr);
    pixels.resize(rampWidth * maxRows * 4);
}

float GradientRamps::rowFor(const Gradient& gradient)
{
    if (gradient.isEmpty())
        return -1.f;

    auto stops = sortedAndClamped(gradient.stops);
    auto key = hashStops(stops);

    // The stop comparison stays: a key is a summary, and two lists can collide.
    for (auto i = 0; i < rows.size(); ++i)
        if (rows[i].key == key && sameStops(rows[i].stops, stops))
            return vForRow(i);

    if (rows.size() >= maxRows)
    {
        ++dropped;
        return -1.f;
    }

    auto row = rows.size();
    rows.add({stops, key});
    bake(rows[row], row);

    return vForRow(row);
}

void GradientRamps::bake(const Row& source, int row)
{
    auto* out = pixels.data() + (std::size_t) row * rampWidth * 4;

    for (auto x = 0; x < rampWidth; ++x)
    {
        // Spans centre to centre, so t of 0 and 1 land exactly on the end stops.
        auto colour = colourAt(source.stops, (float) x / (float) (rampWidth - 1));

        out[x * 4] = toByte(colour.r);
        out[x * 4 + 1] = toByte(colour.g);
        out[x * 4 + 2] = toByte(colour.b);
        out[x * 4 + 3] = toByte(colour.a);
    }
}

float GradientRamps::vForRow(int row)
{
    return ((float) row + 0.5f) / (float) maxRows;
}

void GradientRamps::commit()
{
    if (uploadedRows >= rows.size())
        return;

    auto first = uploadedRows;
    auto count = rows.size() - first;

    texture->update({0.f, (float) first, (float) rampWidth, (float) count},
                    pixels.data() + (std::size_t) first * rampWidth * 4);

    uploadedRows = rows.size();
}
} // namespace eacp::UI
