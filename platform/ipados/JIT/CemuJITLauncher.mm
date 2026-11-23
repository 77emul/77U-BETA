// SPDX-License-Identifier: MPL-2.0
#import "CemuJITLauncher.h"
#import <objc/message.h>
#import <objc/runtime.h>

static NSError *CemuJITLaunchError(NSInteger code, NSString *description) {
    return [NSError errorWithDomain:@"77U.JIT" code:code
                           userInfo:@{NSLocalizedDescriptionKey: description}];
}

@implementation CemuJITLauncher {
    id _activeExtension;
    NSUUID *_activeRequest;
}

- (void)startWithPort:(uint16_t)port
                token:(NSString *)token
           completion:(void (^)(NSError * _Nullable))completion {
    if (port == 0 || token.length != 72 || _activeExtension) {
        completion(CemuJITLaunchError(1, @"Invalid or duplicate JIT helper request."));
        return;
    }

    NSURL *bundleURL = [[[NSBundle mainBundle] builtInPlugInsURL]
        URLByAppendingPathComponent:@"CemuJITHelper.appex"];
    NSString *identifier = [NSBundle bundleWithURL:bundleURL].bundleIdentifier;
    Class extensionClass = NSClassFromString(@"NSExtension");
    SEL factory = NSSelectorFromString(@"extensionWithIdentifier:excludingDisabledExtensions:error:");
    if (!identifier || !extensionClass || !class_respondsToSelector(object_getClass(extensionClass), factory)) {
        completion(CemuJITLaunchError(2, @"The signed JIT helper is missing or unavailable."));
        return;
    }

    NSError *error = nil;
    using Factory = id (*)(id, SEL, NSString *, BOOL, NSError **);
    id extension = ((Factory)objc_msgSend)(extensionClass, factory, identifier, NO, &error);
    if (!extension) {
        completion(error ?: CemuJITLaunchError(3, @"iPadOS could not initialize the JIT helper."));
        return;
    }

    NSExtensionItem *item = [NSExtensionItem new];
    item.userInfo = @{@"77UJITPort": @(port), @"77UJITToken": token};
    NSArray *items = @[item];
    SEL requestWithError = NSSelectorFromString(@"beginExtensionRequestWithInputItems:error:");
    SEL requestWithoutError = NSSelectorFromString(@"beginRequestWithInputItems:");
    id request = nil;
    if ([extension respondsToSelector:requestWithError]) {
        using Start = id (*)(id, SEL, NSArray *, NSError **);
        request = ((Start)objc_msgSend)(extension, requestWithError, items, &error);
    } else if ([extension respondsToSelector:requestWithoutError]) {
        using Start = id (*)(id, SEL, NSArray *);
        request = ((Start)objc_msgSend)(extension, requestWithoutError, items);
    }

    if (![request isKindOfClass:[NSUUID class]]) {
        completion(error ?: CemuJITLaunchError(4, @"iPadOS rejected the JIT helper request."));
        return;
    }
    _activeExtension = extension;
    _activeRequest = request;
    completion(nil);
}

- (void)invalidate {
    id extension = _activeExtension;
    _activeRequest = nil;
    _activeExtension = nil;
    if (!extension) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(600 * NSEC_PER_MSEC)), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        SEL terminate = NSSelectorFromString(@"_kill:");
        if ([extension respondsToSelector:terminate]) {
            using Kill = void (*)(id, SEL, int);
            ((Kill)objc_msgSend)(extension, terminate, 15);
        }
    });
}
@end

extern "C" CFTypeRef SecTaskCreateFromSelf(CFAllocatorRef allocator);
extern "C" CFTypeRef SecTaskCopyValueForEntitlement(CFTypeRef task, CFStringRef entitlement, CFErrorRef *error);

BOOL CemuJITHasGetTaskAllow(void) {
    CFTypeRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
    if (!task) return NO;
    CFTypeRef entitlement = SecTaskCopyValueForEntitlement(task, CFSTR("get-task-allow"), nullptr);
    const BOOL enabled = entitlement && CFEqual(entitlement, kCFBooleanTrue);
    if (entitlement) CFRelease(entitlement);
    CFRelease(task);
    return enabled;
}
