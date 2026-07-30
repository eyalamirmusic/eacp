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

    // Raw bytes, for the component types that are not float or uint16: a
    // normalized colour, a normalized UV, a sparse index list.
    int appendBytes(const std::vector<std::uint8_t>& values)
    {
        auto offset = (int) bytes.size();
        bytes.insert(bytes.end(), values.begin(), values.end());
        return offset;
    }

    int size() const { return (int) bytes.size(); }

    // The buffer as the data URI a glTF's "buffers" entry names.
    std::string asDataUri() const;

    std::vector<std::uint8_t> bytes;
};

// Wraps JSON and binary into a GLB container: the 12-byte header, then a JSON
// chunk and a BIN chunk, each four-byte aligned with the padding its type
// requires — spaces for JSON, zeroes for BIN.
//
// Written here rather than checked in as a fixture for the same reason the .gltf
// documents are: a test that builds its own container says what it is asserting
// about, and the repository keeps no binary blobs.
std::vector<std::uint8_t> makeGlb(const std::string& json,
                                  const std::vector<std::uint8_t>& binary);

// Loads bytes directly, for the cases that are about the container rather than
// about the schema.
LoadResult loadBytes(const std::vector<std::uint8_t>& bytes);

// Whether a case may go on to subscript what it just loaded.
//
// NanoTest's check() records a failure and keeps running, and Vector's
// operator[] is unchecked and noexcept — so a case that asserts a load
// succeeded and then indexes into it *segfaults* when the load fails, instead of
// reporting the failure it was written to report. That is a much worse outcome
// than a red line, because in CI it takes the whole suite's buffered output with
// it and says nothing about which case died.
//
// Every case below that subscripts anything guards on one of these first. Found
// by deliberately breaking the GLB chunk walk and watching a test crash where it
// should have failed.
inline bool hasVertices(const LoadResult& result, int count)
{
    return bool(result) && result.data.vertices.size() >= count;
}

inline bool hasNodes(const LoadResult& result, int count)
{
    return bool(result) && result.data.nodes.size() >= count;
}

inline bool hasPrimitives(const LoadResult& result, int count)
{
    return bool(result) && result.data.primitives.size() >= count;
}

inline bool hasMaterials(const LoadResult& result, int count)
{
    return bool(result) && result.data.materials.size() >= count;
}

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
