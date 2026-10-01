#pragma once

#include "Protobuf.h"

namespace eacp::ML
{
// A written .mlpackage, as bytes: Data/com.apple.CoreML/model.mlmodel (the
// Model.proto), Data/com.apple.CoreML/weights/weight.bin (the MILBlob, absent
// when the program has no tensor constants) and Manifest.json, whose
// identifiers are fixed so a package's bytes are a function of its graph.
struct Package
{
    Bytes model;
    Bytes weights;
    std::string manifest;

    bool isEmpty() const { return model.empty(); }

    // Deletes whatever is at the path first, so refuses one that exists and
    // does not end in .mlpackage.
    bool write(const FilePath& mlpackageDirectory) const;

    static std::string standardManifest();
};
} // namespace eacp::ML
