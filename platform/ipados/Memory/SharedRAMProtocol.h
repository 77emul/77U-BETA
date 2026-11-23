// SPDX-License-Identifier: MPL-2.0
#pragma once
#import <Foundation/Foundation.h>
#import <xpc/xpc.h>
#include <stdint.h>

namespace CemuSharedRAM {
constexpr uint64_t ProtocolVersion = 1;
constexpr size_t MEM2Bytes = 0x40000000ull;
constexpr uint64_t ProbeInitial = 0x7730554d454d3201ull;
constexpr uint64_t ProbeResponse = 0x7730554d454d3202ull;

// These private endpoint accessors are limited to this optional transport.
// No Foundation class implementation or decoder allow-list is modified.
NSXPCListenerEndpoint* BoxEndpoint(xpc_endpoint_t endpoint);
xpc_endpoint_t UnboxEndpoint(NSXPCListenerEndpoint* endpoint);
}

@interface CemuSharedRAMContext : NSExtensionContext
+ (NSSet<Class>*)_allowedItemPayloadClasses;
@end
