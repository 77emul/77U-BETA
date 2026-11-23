// SPDX-License-Identifier: MPL-2.0
#import "SharedRAM.h"
#import "SharedRAMProtocol.h"
#import "SharedRAMMapping.h"
#import <objc/message.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include "Cemu/Logging/CemuLogging.h"

namespace {
std::mutex statusMutex;
NSString* statusText = @"Off - standard guest memory";
std::atomic_bool attempted{false};
void SetStatus(NSString* text)
{
    { std::lock_guard lock(statusMutex); statusText = [text copy]; }
    cemuLog_log(LogType::Force, "[SHARED RAM] {}", text.UTF8String);
}
}

// Private extension entry points, scoped to our own signed helper. Each call is
// capability-checked. Unsupported OS/signing combinations keep standard MEM2.
@interface NSObject (CemuSharedRAMExtension)
+ (id)extensionWithIdentifier:(NSString*)identifier error:(NSError**)error;
- (void)beginExtensionRequestWithInputItems:(NSArray*)items completion:(void (^)(NSUUID*))completion;
- (void)cancelExtensionRequestWithIdentifier:(NSUUID*)identifier;
@end

@interface CemuSharedRAMSession : NSObject
@property(nonatomic, strong) NSString* nonce;
@property(nonatomic, strong) dispatch_semaphore_t ready;
@property(atomic, strong) xpc_connection_t listener;
@property(atomic, strong) xpc_connection_t channel;
@property(atomic, strong) id extension;
@property(atomic, strong) NSUUID* request;
@property(atomic) BOOL closing;
@property(nonatomic, strong) xpc_object_t storage;
@property(nonatomic) void* guestAddress;
@property(nonatomic, strong) NSString* failureReason;
- (bool)mapAt:(void*)address bytes:(size_t)bytes;
- (void)stopTransport;
@end

@implementation CemuSharedRAMSession
- (instancetype)init
{
    if ((self = [super init])) {
        _nonce = NSUUID.UUID.UUIDString;
        _ready = dispatch_semaphore_create(0);
    }
    return self;
}

- (void)startExtension:(NSXPCListenerEndpoint*)endpoint
{
    if (self.closing) return;
    @try {
        NSURL* url = [NSBundle.mainBundle.builtInPlugInsURL URLByAppendingPathComponent:@"CemuSharedRAMHelper.appex"];
        NSString* identifier = [NSBundle bundleWithURL:url].bundleIdentifier;
        Class type = NSClassFromString(@"NSExtension");
        if (!identifier || ![type respondsToSelector:@selector(extensionWithIdentifier:error:)]) return;
        NSError* error = nil;
        id extension = [type extensionWithIdentifier:identifier error:&error];
        if (!extension || ![extension respondsToSelector:@selector(beginExtensionRequestWithInputItems:completion:)]) return;
        self.extension = extension;
        NSExtensionItem* item = [NSExtensionItem new];
        item.userInfo = @{@"version": @(CemuSharedRAM::ProtocolVersion), @"nonce": self.nonce, @"endpoint": endpoint};
        [extension beginExtensionRequestWithInputItems:@[item] completion:^(NSUUID* request) {
            self.request = request;
            if (self.closing) [self cancelRequest];
        }];
    } @catch (NSException* exception) {
        // Decoder rejection is a supported fallback, never worked around by
        // swizzling Foundation or admitting arbitrary decoded classes.
        SetStatus([NSString stringWithFormat:@"Standard memory - helper rejected (%@)", exception.name]);
    }
}

- (void)cancelRequest
{
    id extension = self.extension;
    NSUUID* request = self.request;
    if (request && [extension respondsToSelector:@selector(cancelExtensionRequestWithIdentifier:)])
        [extension cancelExtensionRequestWithIdentifier:request];
}

- (void)stopTransport
{
    self.closing = YES;
    if (self.channel) xpc_connection_cancel(self.channel);
    if (self.listener) xpc_connection_cancel(self.listener);
    dispatch_async(dispatch_get_main_queue(), ^{ [self cancelRequest]; });
}

- (xpc_object_t)requestOperation:(uint64_t)operation
{
    xpc_connection_t channel = self.channel;
    if (!channel || self.closing) return nil;
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block xpc_object_t result = nil;
    NSObject* replyLock = [NSObject new];
    xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(request, "version", CemuSharedRAM::ProtocolVersion);
    xpc_dictionary_set_uint64(request, "operation", operation);
    xpc_dictionary_set_uint64(request, "bytes", CemuSharedRAM::MEM2Bytes);
    xpc_connection_send_message_with_reply(channel, request,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(xpc_object_t reply) {
            if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
                xpc_dictionary_get_uint64(reply, "version") == CemuSharedRAM::ProtocolVersion) { @synchronized(replyLock) { result = reply; } }
            dispatch_semaphore_signal(answered);
        });
    if (dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0) return nil;
    @synchronized(replyLock) { return result; }
}

- (bool)mapAt:(void*)address bytes:(size_t)bytes
{
    // Never block the main thread: it must service the extension launch.
    if (NSThread.isMainThread || bytes != CemuSharedRAM::MEM2Bytes ||
        !CemuSharedRAM::IsAlignedRange(address, bytes, getpagesize())) return false;
    self.listener = xpc_connection_create(nullptr, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!self.listener) return false;
    __weak CemuSharedRAMSession* weakSelf = self;
    xpc_connection_set_event_handler(self.listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
            CemuSharedRAMSession* owner = weakSelf;
            if (!owner || xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
            const char* nonce = xpc_dictionary_get_string(message, "nonce");
            @synchronized (owner) {
                if (owner.closing || owner.channel || !nonce ||
                    xpc_dictionary_get_uint64(message, "version") != CemuSharedRAM::ProtocolVersion ||
                    strcmp(nonce, owner.nonce.UTF8String) != 0) return;
                owner.channel = peer;
                dispatch_semaphore_signal(owner.ready);
            }
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(self.listener);
    NSXPCListenerEndpoint* endpoint = CemuSharedRAM::BoxEndpoint(xpc_endpoint_create(self.listener));
    if (!endpoint) return false;
    dispatch_async(dispatch_get_main_queue(), ^{ [self startExtension:endpoint]; });
    if (dispatch_semaphore_wait(self.ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) return false;
    xpc_object_t reply = [self requestOperation:1];
    if (!reply) return false;
    const char* reason = xpc_dictionary_get_string(reply, "reason");
    if (reason) self.failureReason = [NSString stringWithUTF8String:reason];
    if (xpc_dictionary_get_uint64(reply, "bytes") != bytes) return false;
    xpc_object_t storage = xpc_dictionary_get_value(reply, "storage");
    if (!storage || xpc_get_type(storage) != XPC_TYPE_SHMEM) return false;
    void* staging = nullptr;
    const size_t mapped = xpc_shmem_map(storage, &staging);
    if (!staging || mapped != bytes) {
        if (staging && mapped) munmap(staging, mapped);
        return false;
    }
    bool success = false;
    if (CemuSharedRAM::VerifyProbe(staging, bytes, CemuSharedRAM::ProbeInitial)) {
        CemuSharedRAM::WriteProbe(staging, bytes, CemuSharedRAM::ProbeResponse);
        xpc_object_t verified = [self requestOperation:2];
        if (verified && xpc_dictionary_get_bool(verified, "verified") &&
            CemuSharedRAM::VerifyProbe(staging, bytes, 0)) {
            // One final remap publishes the whole verified allocation. No
            // partially installed chunks and no changes to live guest bytes.
            success = CemuSharedRAM::MapBacking(address, staging, bytes);
        }
    }
    munmap(staging, mapped);
    if (success) { self.storage = storage; self.guestAddress = address; }
    return success;
}
@end

static CemuSharedRAMSession* activeSession;

static NSString* SharedRAMEnvironmentKey();
static NSString* const kSharedRAMUnavailableKey = @"CemuIOS.SharedRAMUnavailableEnvironment";

NSString* CemuSharedRAMStatus(void)
{
    if (!attempted.load())
    {
        if (![NSUserDefaults.standardUserDefaults boolForKey:@"CemuIOS.ExperimentalSharedRAM"])
            return @"Off - standard guest memory";
        if ([[NSUserDefaults.standardUserDefaults stringForKey:kSharedRAMUnavailableKey] isEqualToString:SharedRAMEnvironmentKey()])
            return @"Unavailable on this iOS version - standard guest memory";
        return @"Ready - applies at the next game launch";
    }
    std::lock_guard lock(statusMutex);
    return statusText;
}

// The helper launch can only fail by timing out (5 s per game launch). Remember
// a failure for this app build on this OS version and skip it afterwards; a
// new build or OS update tries once again.
static NSString* SharedRAMEnvironmentKey()
{
    NSString* build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    return [NSString stringWithFormat:@"%@|%@", build, NSProcessInfo.processInfo.operatingSystemVersionString];
}

bool CemuSharedRAM_TryMap(void* destination, size_t bytes)
{
    if (![NSUserDefaults.standardUserDefaults boolForKey:@"CemuIOS.ExperimentalSharedRAM"]) return false;
    if (bytes != CemuSharedRAM::MEM2Bytes) {
        SetStatus(@"Standard memory - custom MEM2 size");
        return false;
    }
    if ([[NSUserDefaults.standardUserDefaults stringForKey:kSharedRAMUnavailableKey] isEqualToString:SharedRAMEnvironmentKey()]) {
        attempted = true;
        SetStatus(@"Standard memory - helper unavailable on this iOS version (not retried until update)");
        return false;
    }
    if (attempted.exchange(true)) return false;
    CemuSharedRAMSession* session = [CemuSharedRAMSession new];
    bool mapped = false;
    @try { mapped = [session mapAt:destination bytes:bytes]; }
    @catch (NSException* exception) { mapped = false; }
    if (mapped) {
        activeSession = session;
        SetStatus(@"Active - 1 GiB shared Wii U MEM2; accounting benefit unverified");
    } else {
        [session stopTransport];
        if (!session.failureReason) // no answer from the helper at all
            [NSUserDefaults.standardUserDefaults setObject:SharedRAMEnvironmentKey() forKey:kSharedRAMUnavailableKey];
        SetStatus(session.failureReason
            ? [@"Standard memory - " stringByAppendingString:session.failureReason]
            : @"Standard memory - shared RAM unavailable; restart to retry");
    }
    return mapped;
}

void CemuSharedRAM_DidUnmap(void* destination)
{
    // MMU has already made this range inaccessible, and all guest work is done.
    // Keep the address reservation intact; never deallocate the guest VA here.
    if (!activeSession || activeSession.guestAddress != destination) return;
    [activeSession stopTransport];
    activeSession.storage = nil;
    activeSession = nil;
    SetStatus(@"Stopped - restart before using shared RAM again");
}
