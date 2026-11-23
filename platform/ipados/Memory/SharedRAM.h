// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
// Called only for a fresh, inaccessible MEM2 reservation, before title loading.
bool CemuSharedRAM_TryMap(void* _Nullable destination, size_t bytes);
// Guest cores and the renderer must have stopped using MEM2 before this call.
void CemuSharedRAM_DidUnmap(void* _Nullable destination);
#ifdef __cplusplus
}
#endif

#ifdef __OBJC__
#import <Foundation/Foundation.h>
FOUNDATION_EXPORT NSString* _Nonnull CemuSharedRAMStatus(void);
#endif
