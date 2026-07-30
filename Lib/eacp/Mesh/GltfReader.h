#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/FilePath.h>

#include <Miro/Json.h>

#include <cstdint>
#include <span>
#include <string>

// How to get numbers out of a glTF file. GltfLoader is what turns them into a
// MeshData; this is everything below that: the container, the buffers, and the
// accessor indirection the spec defines.
//
// Written on Miro::Json rather than against a third-party parser, so the whole
// path from bytes to geometry is code in this tree. The JSON itself is the small
// part - what this file mostly is, is the layer between "the file says accessor
// 3" and "here are 72 floats": five component types, normalization, a byte
// stride that may or may not be given, and sparse overrides.
//
// Every read is bounds-checked and every failure is a return value. A model file
// is untrusted input, and Miro::Json's own accessors throw on a type mismatch
// (std::get on the variant, .at() on the object) - so nothing here calls them
// directly. That is what the safe readers at the top exist for.

namespace eacp::Mesh
{
namespace Gltf
{
// A field of an object, or null when the object does not have it or is not an
// object at all. Every other reader is built on this one.
const Miro::Json::Value* member(const Miro::Json::Value& value,
                                std::string_view key);

// Typed reads with a fallback for absent-or-wrong-type, which is what a glTF
// default is: "material.doubleSided" missing and "material.doubleSided": "yes"
// both mean the model does not say, and the spec's default applies.
int intOr(const Miro::Json::Value& value, std::string_view key, int fallback);
float floatOr(const Miro::Json::Value& value, std::string_view key, float fallback);
bool boolOr(const Miro::Json::Value& value, std::string_view key, bool fallback);
std::string stringOr(const Miro::Json::Value& value,
                     std::string_view key,
                     std::string_view fallback = {});

// The array at a key, or null. Empty and absent are deliberately different here:
// a caller iterating "meshes" wants to know the difference between a file with
// no meshes and one whose "meshes" is a string.
const Miro::Json::Array* arrayMember(const Miro::Json::Value& value,
                                     std::string_view key);

// Element `index` of an array, or null when out of range. glTF is a graph of
// integer indices into top-level arrays, and an out-of-range one is the most
// common way a hand-edited file is broken.
const Miro::Json::Value* at(const Miro::Json::Array* array, int index);

// Fills `out` from a JSON array of numbers, up to `count` of them, and returns
// how many were written. Used for the fixed-width things glTF spells as arrays:
// a base colour factor, a node's translation, a 4x4 matrix.
int readNumbers(const Miro::Json::Value& value,
                std::string_view key,
                float* out,
                int count);

// glTF's componentType enum. Named rather than left as the numbers so the
// switch that reads them cannot silently gain a wrong case.
enum class ComponentType
{
    Invalid = 0,
    Byte = 5120,
    UnsignedByte = 5121,
    Short = 5122,
    UnsignedShort = 5123,
    UnsignedInt = 5125,
    Float = 5126
};

int bytesPerComponent(ComponentType type);

// How many components glTF's "type" string means. Zero for one this reader does
// not handle, which is every matrix type: no vertex attribute we read is a
// matrix, and the spec's column padding for byte and short matrices is a trap
// worth refusing rather than half-implementing.
int componentsForType(std::string_view type);

// One accessor's worth of data, expanded.
//
// Expanded rather than read lazily because sparse accessors override individual
// elements, and applying that during a strided walk is where the subtle bugs
// are. The cost is one temporary array per attribute, freed as soon as the
// vertices are packed - transient, and bounded by the attribute rather than the
// model.
struct FloatData
{
    bool isValid() const { return count > 0 && components > 0; }

    // Element `element`, component `component`. Out of range reads zero, so a
    // caller that miscounts gets a wrong picture rather than a crash.
    float get(int element, int component) const;

    Vector<float> values;
    int count = 0;
    int components = 0;
};

// A glTF or GLB file, parsed, with its buffers resolved.
//
// The two forms differ only in how the JSON and the binary arrive - one file
// with chunks, or JSON plus separate buffers - so the difference is handled once
// here and nothing above this sees it.
class Document
{
public:
    // basePath is the directory external buffers and images resolve against.
    // Empty means a file with none is loaded for its geometry and gets no
    // textures, which is what loadGltfFromMemory promises.
    bool parse(std::span<const std::uint8_t> bytes, const FilePath& basePath);

    const std::string& error() const { return errorText; }

    const Miro::Json::Value& root() const { return rootValue; }

    // The top-level array of one of glTF's object kinds, or null.
    const Miro::Json::Array* collection(std::string_view key) const;

    // Reads accessor `index` as floats. `maxComponents` caps how many are kept
    // per element, so a caller wanting a position from a VEC4 accessor gets
    // three rather than a size mismatch.
    FloatData readFloats(int index, int maxComponents) const;

    // Reads accessor `index` as indices, whatever width the file stored them
    // at. Empty when the accessor is missing or is not scalar.
    Vector<std::uint32_t> readIndices(int index) const;

    // The bytes of a buffer view, for the one thing that is not an accessor: an
    // image embedded in the binary chunk.
    std::span<const std::uint8_t> bufferViewBytes(int index) const;

    // Resolves an image's `uri`: a base64 data URI decoded here, or a file read
    // relative to basePath. Empty when there is neither.
    Vector<std::uint8_t> readUri(std::string_view uri) const;

private:
    struct BufferView
    {
        int buffer = -1;
        int byteOffset = 0;
        int byteLength = 0;
        int byteStride = 0;
    };

    struct Accessor
    {
        int bufferView = -1;
        int byteOffset = 0;
        ComponentType componentType = ComponentType::Invalid;
        bool normalized = false;
        int count = 0;
        int components = 0;

        bool hasSparse = false;
        int sparseCount = 0;
        int sparseIndexView = -1;
        int sparseIndexOffset = 0;
        ComponentType sparseIndexType = ComponentType::Invalid;
        int sparseValueView = -1;
        int sparseValueOffset = 0;
    };

    bool splitGlb(std::span<const std::uint8_t> bytes,
                  std::string_view& json,
                  std::span<const std::uint8_t>& binary);

    // Resolves every buffer's bytes. basePath is not a parameter because
    // readUri already resolves against the member `base` that parse() set.
    bool resolveBuffers(std::span<const std::uint8_t> glbBinary);
    void resolveViews();
    bool resolveAccessors();

    // Reads one element of an accessor out of its buffer view, at the stride the
    // view declares or the tight packing it implies.
    bool readElements(const Accessor& accessor,
                      int viewIndex,
                      int byteOffset,
                      int firstElement,
                      int elementCount,
                      int maxComponents,
                      float* out) const;

    void applySparse(const Accessor& accessor,
                     int maxComponents,
                     FloatData& data) const;

    Miro::Json::Value rootValue;
    std::string errorText;

    FilePath base;

    Vector<Vector<std::uint8_t>> buffers;
    Vector<BufferView> views;
    Vector<Accessor> accessors;
};

// Base64 as a data URI carries it. Returns empty on anything that is not valid
// base64, rather than a partial decode - a truncated buffer is worse than none.
Vector<std::uint8_t> decodeBase64(std::string_view encoded);

// Percent-escapes, in place of the URI they came from: "chair%20base.png" is a
// file whose name has a space in it, and looking for the unescaped name finds
// nothing.
std::string decodePercentEscapes(std::string_view uri);
} // namespace Gltf
} // namespace eacp::Mesh
