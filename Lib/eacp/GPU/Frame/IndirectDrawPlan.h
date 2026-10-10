#pragma once

#include "../Buffer/Buffer.h"

#include <cstdint>
#include <optional>

namespace eacp::GPU
{
// A run of indirect draws the checks passed: where the first record starts,
// how many there are, and the byte step between them. Stride is never zero
// here; a caller's zero means tightly packed and is resolved to the record size.
struct IndirectDrawPlan
{
    std::int64_t offset = 0;
    int drawCount = 0;
    std::int64_t stride = 0;
};

// Offsets and strides on four bytes, a stride no shorter than one record, and
// every record inside the buffer. Nothing to draw (drawCount below one) is an
// empty optional with no trace; anything else refused is traced.
std::optional<IndirectDrawPlan> planIndirectDraws(const Buffer& arguments,
                                                  std::int64_t offset,
                                                  int drawCount,
                                                  std::int64_t stride,
                                                  std::int64_t recordBytes);

// The count an indirect-count draw reads: one uint32 at a four-byte offset
// inside the buffer.
bool isValidDrawCountBuffer(const Buffer& count, std::int64_t countOffset);

// The trace the backends without a GPU-side draw count leave behind.
void traceIndirectCountUnsupported();
} // namespace eacp::GPU
