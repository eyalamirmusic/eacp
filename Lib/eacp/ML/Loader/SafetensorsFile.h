#pragma once

#include "../../Core/Utils/MemoryMappedFile.h"
#include "../Tensor/Tensor.h"

#include <cstdint>
#include <map>
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

    float loadScalar(const std::string& name) const;

private:
    SafetensorsFile(MemoryMappedFile mappedFile,
                    std::uint64_t dataStart,
                    std::map<std::string, SafetensorsEntry> entriesToUse);

    MemoryMappedFile mapped;
    std::uint64_t dataSectionStart = 0;
    std::map<std::string, SafetensorsEntry> entries;
};
}
