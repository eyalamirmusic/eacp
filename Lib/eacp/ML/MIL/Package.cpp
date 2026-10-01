#include "Package.h"

#include <exception>

namespace eacp::ML
{
namespace
{
constexpr auto packageModelIdentifier = "3E9F4D2A-6C1B-4E57-9A8D-0C2B5F7E1A01";
constexpr auto packageWeightsIdentifier = "3E9F4D2A-6C1B-4E57-9A8D-0C2B5F7E1A02";

std::string manifestItem(std::string_view identifier,
                         std::string_view description,
                         std::string_view name,
                         std::string_view path)
{
    auto text = std::string {"        \""};
    text += identifier;
    text += "\": {\n            \"author\": \"com.apple.CoreML\",\n";
    text += "            \"description\": \"";
    text += description;
    text += "\",\n            \"name\": \"";
    text += name;
    text += "\",\n            \"path\": \"";
    text += path;
    text += "\"\n        }";
    return text;
}

bool isSafeToReplace(const FilePath& mlpackageDirectory)
{
    return mlpackageDirectory.extension() == ".mlpackage"
           || !File {mlpackageDirectory}.exists();
}
} // namespace

std::string Package::standardManifest()
{
    auto text = std::string {"{\n    \"fileFormatVersion\": \"1.0.0\",\n"
                             "    \"itemInfoEntries\": {\n"};
    text += manifestItem(packageModelIdentifier,
                         "CoreML Model Specification",
                         "model.mlmodel",
                         "com.apple.CoreML/model.mlmodel");
    text += ",\n";
    text += manifestItem(packageWeightsIdentifier,
                         "CoreML Model Weights",
                         "weights",
                         "com.apple.CoreML/weights");
    text += "\n    },\n    \"rootModelIdentifier\": \"";
    text += packageModelIdentifier;
    text += "\"\n}\n";
    return text;
}

bool Package::write(const FilePath& mlpackageDirectory) const
{
    if (isEmpty() || mlpackageDirectory.empty()
        || !isSafeToReplace(mlpackageDirectory))
        return false;

    auto data = mlpackageDirectory / "Data" / "com.apple.CoreML";
    auto weightsDirectory = data / "weights";

    if (!Files::removeAll(mlpackageDirectory)
        || !Files::createDirectories(weightsDirectory))
        return false;

    try
    {
        auto manifestText = manifest.empty() ? standardManifest() : manifest;
        Files::writeFile(mlpackageDirectory / "Manifest.json",
                         asBytes(manifestText));
        Files::writeFile(data / "model.mlmodel", model);

        if (!weights.empty())
            Files::writeFile(weightsDirectory / "weight.bin", weights);

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace eacp::ML
