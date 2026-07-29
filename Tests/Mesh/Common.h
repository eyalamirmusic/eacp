#pragma once

#include <eacp/Mesh/Mesh.h>

#include <NanoTest/NanoTest.h>

#include <string>
#include <vector>

// Shared scaffolding for the Mesh tests: a way to author a glTF document inside
// a test, so nothing here depends on a model file on disk.
//
// That is deliberate rather than convenient. A test that loads a checked-in
// .glb proves the loader still does whatever it did when the file was added; a
// test that writes the glTF beside the assertion says what it is asserting
// about. It also keeps the repository free of binary fixtures.

namespace eacp::Mesh::Tests
{
// The bytes a glTF's single buffer holds, assembled a chunk at a time. Each
// append returns the byte offset a bufferView should point at.
class BinaryBuffer
{
public:
    int appendFloats(const std::vector<float>& values)
    {
        auto offset = (int) bytes.size();

        for (auto value: values)
        {
            const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(float));
        }

        return offset;
    }

    int appendIndices(const std::vector<std::uint16_t>& values)
    {
        auto offset = (int) bytes.size();

        for (auto value: values)
        {
            const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(std::uint16_t));
        }

        return offset;
    }

    int size() const { return (int) bytes.size(); }

    // The buffer as the data URI a glTF's "buffers" entry names.
    std::string asDataUri() const;

    std::vector<std::uint8_t> bytes;
};

// Parses a document written as JSON text, with `binary` supplied as the single
// buffer. The "@BUFFER@" token in the JSON is replaced with the data URI, and
// "@BYTELENGTH@" with its length, so a test writes the structure it cares about
// and neither of those by hand.
LoadResult loadDocument(const std::string& json, const BinaryBuffer& binary);

// A cube's 24 positions and 36 indices, wound counter-clockwise when seen from
// outside - glTF's convention, and so the winding face culling keeps by
// default. Returned rather than authored per test because three of the cases
// need geometry that is closed, and a cube that is wound wrongly makes a
// culling test fail for a reason that has nothing to do with culling.
std::vector<float> cubePositions();
std::vector<std::uint16_t> cubeIndices();

// Whether two floats agree to a tolerance the test can state. Written out
// rather than pulled from a helper so each call site says how much slack it is
// allowing and why.
inline bool near(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

inline bool near(Vec3 a, Vec3 b, float tolerance)
{
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance)
           && near(a.z, b.z, tolerance);
}
} // namespace eacp::Mesh::Tests
