// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <mach/mach.h>

namespace CemuSharedRAM {
bool IsAlignedRange(void* address, size_t bytes, size_t pageSize);
// Nonvolatile purgeable VM: ownership can be accounted separately, but guest
// bytes must never be declared volatile or discarded under pressure.
bool CreateBacking(size_t bytes, void** result);
bool VerifyProbe(void* address, size_t bytes, uint64_t value);
void WriteProbe(void* address, size_t bytes, uint64_t value);
// Map at the guest's existing address, without copying or changing JIT dispatch.
// Caller owns the fresh destination reservation and guarantees no live users.
bool MapBacking(void* destination, void* source, size_t bytes);
}
