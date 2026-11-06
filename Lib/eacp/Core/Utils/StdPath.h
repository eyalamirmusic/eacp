#pragma once

/** @file
    Internal: conversion between FilePath and std::filesystem::path, for .cpp files.
*/

#include "FilePath.h"

#include <filesystem>

// The FilePath -> std::filesystem boundary, for implementation files and
// callers that need real path algebra. Public headers only see FilePath.
namespace eacp
{
std::filesystem::path toStdPath(const FilePath& path);
} // namespace eacp
