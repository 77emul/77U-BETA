// SPDX-License-Identifier: MPL-2.0
#include "SharedRAMMapping.h"
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

// The iOS SDK does not ship mach_vm.h; these are the libSystem Mach interfaces.
extern "C" kern_return_t mach_vm_allocate(vm_map_t, mach_vm_address_t*, mach_vm_size_t, int);
extern "C" kern_return_t mach_vm_deallocate(vm_map_t, mach_vm_address_t, mach_vm_size_t);
extern "C" kern_return_t mach_vm_remap(vm_map_t, mach_vm_address_t*, mach_vm_size_t,
    mach_vm_offset_t, int, vm_map_t, mach_vm_address_t, boolean_t,
    vm_prot_t*, vm_prot_t*, vm_inherit_t);

bool CemuSharedRAM::IsAlignedRange(void* address, size_t bytes, size_t pageSize)
{
    const auto start = reinterpret_cast<uintptr_t>(address);
    return start && bytes && pageSize && start % pageSize == 0 &&
        bytes % pageSize == 0 && bytes <= UINTPTR_MAX - start;
}

bool CemuSharedRAM::CreateBacking(size_t bytes, void** result)
{
    if (!result) return false;
    *result = nullptr;
    const size_t pageSize = getpagesize();
    if (!bytes || bytes % pageSize) return false;
    mach_vm_address_t address = 0;
    if (mach_vm_allocate(mach_task_self(), &address, bytes,
            VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
    // Required guest memory is never purgeable, including while the extension
    // is suspended or under memory pressure. This does not raise jetsam limits.
    *result = reinterpret_cast<void*>(address);
    return true;
}

bool CemuSharedRAM::VerifyProbe(void* address, size_t bytes, uint64_t value)
{
    if (!address || bytes < 2 * sizeof(value)) return false;
    uint64_t first, last;
    memcpy(&first, address, sizeof(first));
    memcpy(&last, static_cast<char*>(address) + bytes - sizeof(last), sizeof(last));
    return first == value && last == value;
}

void CemuSharedRAM::WriteProbe(void* address, size_t bytes, uint64_t value)
{
    if (!address || bytes < 2 * sizeof(value)) return;
    memcpy(address, &value, sizeof(value));
    memcpy(static_cast<char*>(address) + bytes - sizeof(value), &value, sizeof(value));
}

bool CemuSharedRAM::MapBacking(void* destination, void* source, size_t bytes)
{
    const size_t pageSize = getpagesize();
    if (!IsAlignedRange(destination, bytes, pageSize) || !IsAlignedRange(source, bytes, pageSize)) return false;
    const auto dst = reinterpret_cast<uintptr_t>(destination);
    const auto src = reinterpret_cast<uintptr_t>(source);
    if (dst < src + bytes && src < dst + bytes) return false;
    mach_vm_address_t address = dst;
    vm_prot_t current = VM_PROT_NONE, maximum = VM_PROT_NONE;
    const auto result = mach_vm_remap(mach_task_self(), &address, bytes, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), src, FALSE,
        &current, &maximum, VM_INHERIT_SHARE);
    // Both sides originate as RW, non-executable VM. No extra protection change
    // (or failure point) after replacing the destination reservation.
    return result == KERN_SUCCESS;
}
