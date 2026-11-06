#pragma once
/** @file RAII Objective-C autorelease pool. Apple only. */

namespace eacp::ObjC
{
// Pushes a pool on construction and drains it on destruction; keep it on the stack.
class AutoReleasePool
{
public:
    AutoReleasePool();
    ~AutoReleasePool();

    AutoReleasePool(const AutoReleasePool&) = delete;

private:
    void* context {nullptr};
};
} // namespace eacp::ObjC
