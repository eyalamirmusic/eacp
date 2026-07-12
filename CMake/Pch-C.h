#pragma once

// The C counterpart of Pch.h, deliberately empty.
//
// CMake emits a PCH for every language its consumers compile, and the
// generated resource blobs are C -- but a C translation unit cannot
// precompile the C++ standard library, and those blobs are plain data with
// nothing worth precompiling anyway.
