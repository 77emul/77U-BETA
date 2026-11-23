// SPDX-License-Identifier: MPL-2.0
#import "SharedRAMProtocol.h"
#import <objc/message.h>

@implementation CemuSharedRAMContext
+ (NSSet<Class>*)_allowedItemPayloadClasses
{
    // Only the classes used by our extension request; secure decoding stays on.
    return [NSSet setWithObjects:NSExtensionItem.class, NSArray.class,
        NSDictionary.class, NSString.class, NSNumber.class,
        NSXPCListenerEndpoint.class, nil];
}
@end

NSXPCListenerEndpoint* CemuSharedRAM::BoxEndpoint(xpc_endpoint_t endpoint)
{
    NSXPCListenerEndpoint* boxed = [NSXPCListenerEndpoint new];
    SEL setter = NSSelectorFromString(@"_setEndpoint:");
    if (![boxed respondsToSelector:setter]) return nil;
    using Set = void (*)(id, SEL, xpc_object_t);
    reinterpret_cast<Set>(objc_msgSend)(boxed, setter, endpoint);
    return boxed;
}

xpc_endpoint_t CemuSharedRAM::UnboxEndpoint(NSXPCListenerEndpoint* endpoint)
{
    if (![endpoint isKindOfClass:NSXPCListenerEndpoint.class]) return nil;
    SEL getter = NSSelectorFromString(@"_endpoint");
    if (![endpoint respondsToSelector:getter]) return nil;
    using Get = xpc_object_t (*)(id, SEL);
    xpc_object_t value = reinterpret_cast<Get>(objc_msgSend)(endpoint, getter);
    return value && xpc_get_type(value) == XPC_TYPE_ENDPOINT ? value : nil;
}
