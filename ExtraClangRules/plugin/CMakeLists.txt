cmake_minimum_required(VERSION 3.31)
project(EacpTidyChecks CXX)

# Built against a pinned LLVM (see ../README.md): clang-tidy plugins have no
# ABI stability guarantee, so this library must be loaded by the clang-tidy
# from the same LLVM it was compiled against.
#
#   cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm@21
#   cmake --build build

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Clang REQUIRED CONFIG)

add_library(EacpTidyChecks MODULE EacpTidyChecks.cpp)

target_compile_features(EacpTidyChecks PRIVATE cxx_std_20)
target_include_directories(EacpTidyChecks SYSTEM PRIVATE
        ${CLANG_INCLUDE_DIRS}
        ${LLVM_INCLUDE_DIRS})
target_compile_options(EacpTidyChecks PRIVATE -fno-rtti)

if (APPLE)
    # Symbols resolve against the hosting clang-tidy binary at --load time.
    target_link_options(EacpTidyChecks PRIVATE -undefined dynamic_lookup)
endif ()
