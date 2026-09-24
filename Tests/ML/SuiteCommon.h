#pragma once

#include <eacp/Core/Utils/StdPath.h>
#include <eacp/ML/ML.h>

#if EACP_HAS_COREML
#include <filesystem>
#include <string>
#include <unistd.h>

namespace MLSuiteCommon
{
struct ScratchRoot
{
    ~ScratchRoot()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(eacp::toStdPath(path), error);
    }

    eacp::FilePath path = eacp::FilePath::tempDirectory()
                          / ("eacp-ml-tests-" + std::to_string(getpid()));
};

inline const eacp::FilePath& scratchRoot()
{
    static auto root = ScratchRoot {};
    return root.path;
}

inline eacp::FilePath freshScratchDirectory(const std::string& name)
{
    auto directory = scratchRoot() / name;
    auto error = std::error_code {};
    std::filesystem::remove_all(eacp::toStdPath(directory), error);
    return directory;
}

inline eacp::ML::MultiArray arrayOf(const eacp::Vector<float>& values,
                                    const eacp::ML::Shape& shape,
                                    eacp::ML::DType type)
{
    auto array = eacp::ML::MultiArray::create(shape, type);
    array.fromFloats(values);
    return array;
}
} // namespace MLSuiteCommon
#endif
