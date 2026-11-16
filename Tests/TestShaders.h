#pragma once

#include <ResEmbed/ResEmbed.h>

#include <string>

// A hand-written shader a suite carries as a file under its Shaders/ directory,
// embedded by eacp_embed_test_shaders (Tests/CMakeLists.txt) and keyed by its
// path there: "Scissor/green.wgsl". Empty when the suite has no such file.
inline std::string testShaderFile(const std::string& path)
{
    return ResEmbed::get(path, "TestShaders").toString();
}
