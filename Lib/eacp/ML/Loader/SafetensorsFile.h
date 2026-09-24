#pragma once

#include "../../Core/Utils/MemoryMappedFile.h"
#include "../Tensor/Tensor.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eacp::ML
{
enum class SafetensorsDType
{
    F32,
    F16,
    BF16,
    Unknown
};

struct SafetensorsEntry
{
    SafetensorsDType dtype = SafetensorsDType::Unknown;
    std::vector<int> shape;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteLength = 0;
};

// A safetensors checkpoint, memory-mapped rather than read.
//
// loadF32 is the one call a model's loader makes per tensor, and it gives the
// cheapest tensor the device allows. Where the device can adopt host memory
// (Metal), the first load makes the whole mapping one GPU buffer and every F32
// tensor is a range of it: nothing is copied, the pages come off the disk as
// the GPU first reads them, and the OS can evict them again, since they are the
// file's. That buffer holds the mapping, so the tensors outlive this object
// safely. A tensor whose offset is off the device's storage-bind grid, and
// every tensor on a device that cannot adopt memory, is copied into a buffer
// of its own; F16 and BF16 are converted to F32 on the host. The caller writes
// the same code either way.
//
// Tensors loaded in place are the file's bytes: nothing may write through
// them, which no weight is ever asked to take.
class SafetensorsFile
{
public:
    static std::optional<SafetensorsFile> open(const FilePath& path);

    const std::map<std::string, SafetensorsEntry>& tensors() const { return entries; }
    const SafetensorsEntry* find(const std::string& name) const;

    const std::uint8_t* rawBytes(const std::string& name) const;

    Tensor loadF32(const std::string& name,
                   GPU::Device& device = GPU::Device::shared()) const;

    Tensor loadPackedF16(const std::string& name,
                         GPU::Device& device = GPU::Device::shared()) const;

    // A tensor's values on the host as F32, whatever it is stored as: for the
    // weights a loader transforms before they reach the GPU.
    std::vector<float> readF32(const std::string& name) const;

    float loadScalar(const std::string& name) const;

    // How the F32 loads so far were served: in place, or copied because the
    // device cannot adopt the mapping or the tensor's offset is off its grid;
    // and how many were converted from F16 or BF16.
    struct LoadCounts
    {
        int inPlace = 0;
        int copied = 0;
        int converted = 0;
    };

    LoadCounts loadCounts() const { return counts; }

private:
    SafetensorsFile(MemoryMappedFile mappedFile,
                    std::uint64_t dataStart,
                    std::map<std::string, SafetensorsEntry> entriesToUse);

    std::shared_ptr<const GPU::Buffer> fileBufferFor(GPU::Device& device) const;
    std::int64_t fileOffsetOf(const SafetensorsEntry& entry) const;

    std::shared_ptr<const MemoryMappedFile> mapped;
    std::uint64_t dataSectionStart = 0;
    std::map<std::string, SafetensorsEntry> entries;

    mutable std::shared_ptr<const GPU::Buffer> fileBuffer;
    mutable const GPU::Device* fileBufferDevice = nullptr;
    mutable LoadCounts counts;
};
}
