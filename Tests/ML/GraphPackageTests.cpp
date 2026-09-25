#include "GraphCommon.h"

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

namespace
{
Graph projection(float firstWeight, bool withUnusedConstant = false)
{
    auto graph = Graph {};
    auto x = graph.input("x", {4, 2}, DType::float16);

    if (withUnusedConstant)
        zeroConstant(graph, "unused", {3});

    auto weights = Vector<float> {firstWeight, 0, 0, 1};
    auto weight = graph.halfConstant("w", {2, 2}, weights);
    auto bias = zeroConstant(graph, "b", {2});
    graph.output(graph.softmax(graph.linear(x, weight, bias), -1), "y");
    return graph;
}

FilePath scratchDirectory(std::string_view name)
{
    return FilePath::tempDirectory() / "eacp-MLGraphTests-packages" / name;
}

bool exists(const FilePath& path)
{
    return File {path}.exists();
}

Bytes readBytes(const FilePath& path)
{
    auto file = MemoryMappedFile {path};
    auto bytes = Bytes {};

    for (auto byte: file.bytes())
        bytes.add(byte);

    return bytes;
}
} // namespace

auto tPackageManifest = test("MLGraph/Package/manifestHasFixedIdentifiers") = []
{
    checkText(
        Package::standardManifest(),
        "{\n"
        "    \"fileFormatVersion\": \"1.0.0\",\n"
        "    \"itemInfoEntries\": {\n"
        "        \"3E9F4D2A-6C1B-4E57-9A8D-0C2B5F7E1A01\": {\n"
        "            \"author\": \"com.apple.CoreML\",\n"
        "            \"description\": \"CoreML Model Specification\",\n"
        "            \"name\": \"model.mlmodel\",\n"
        "            \"path\": \"com.apple.CoreML/model.mlmodel\"\n"
        "        },\n"
        "        \"3E9F4D2A-6C1B-4E57-9A8D-0C2B5F7E1A02\": {\n"
        "            \"author\": \"com.apple.CoreML\",\n"
        "            \"description\": \"CoreML Model Weights\",\n"
        "            \"name\": \"weights\",\n"
        "            \"path\": \"com.apple.CoreML/weights\"\n"
        "        }\n"
        "    },\n"
        "    \"rootModelIdentifier\": \"3E9F4D2A-6C1B-4E57-9A8D-0C2B5F7E1A01\"\n"
        "}\n");
};

auto tPackageLayout = test("MLGraph/Package/writeProducesTheDirectoryLayout") = []
{
    auto package = buildChecked(projection(1.f));
    auto directory = scratchDirectory("Projection.mlpackage");

    check(package.write(directory));

    auto data = directory / "Data" / "com.apple.CoreML";
    check(Files::readFile(directory / "Manifest.json")
          == Package::standardManifest());
    check(readBytes(data / "model.mlmodel") == package.model);
    check(readBytes(data / "weights" / "weight.bin") == package.weights);

    auto stray = directory / "stray.txt";
    Files::writeFile(stray, asBytes(std::string_view {"left over"}));
    check(package.write(directory));
    check(!exists(stray));

    Files::removeAll(directory);
};

auto tPackageNoWeights = test("MLGraph/Package/noConstantsMeansNoWeightFile") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3}, DType::float16);
    graph.output(graph.softmax(x, 0), "y");

    auto package = buildChecked(graph);
    auto directory = scratchDirectory("NoWeights.mlpackage");

    check(package.weights.empty());
    check(package.write(directory));
    check(exists(directory / "Data" / "com.apple.CoreML" / "weights"));
    check(
        !exists(directory / "Data" / "com.apple.CoreML" / "weights" / "weight.bin"));

    Files::removeAll(directory);
};

auto tPackageEmpty = test("MLGraph/Package/anEmptyPackageIsNotWritten") = []
{
    auto directory = scratchDirectory("Empty.mlpackage");
    check(!Package {}.write(directory));
    check(!exists(directory));
};

auto tPackageRefusesForeignPath =
    test("MLGraph/Package/refusesToReplaceAnythingButAnMlpackage") = []
{
    auto package = buildChecked(projection(1.f));
    auto directory = scratchDirectory("NotAPackage");
    auto kept = directory / "kept.txt";
    Files::writeFile(kept, asBytes(std::string_view {"keep me"}));

    check(!package.write(directory));
    check(exists(kept));

    Files::removeAll(directory);
    check(package.write(directory));
    check(exists(directory / "Manifest.json"));

    Files::removeAll(directory);
};

auto tPackageDeterministic = test("MLGraph/Package/bytesAreStable") = []
{
    auto first = projection(1.f).build();
    auto second = projection(1.f).build();

    check(first.model == second.model);
    check(first.weights == second.weights);
    check(first.manifest == second.manifest);
};

auto tPackageUnusedConstant =
    test("MLGraph/Package/anUnreferencedConstantIsLeftOut") = []
{
    auto expected = projection(1.f).build();
    auto actual = buildChecked(projection(1.f, true));

    check(actual.model == expected.model);
    check(actual.weights == expected.weights);
};
