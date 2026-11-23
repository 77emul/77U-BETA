// SPDX-License-Identifier: MPL-2.0
#import "SharedRAMProtocol.h"
#import "SharedRAMMapping.h"
#include <sys/mman.h>
#include <os/proc.h>

@interface CemuSharedRAMRequestHandler : NSObject <NSExtensionRequestHandling>
@end

@implementation CemuSharedRAMRequestHandler {
    NSExtensionContext* _context;
    xpc_connection_t _connection;
    void* _backing;
    size_t _bytes;
    BOOL _verified;
}

- (void)beginRequestWithExtensionContext:(NSExtensionContext*)context
{
    _context = context;
    NSExtensionItem* item = context.inputItems.firstObject;
    if (![item isKindOfClass:NSExtensionItem.class]) return;
    NSDictionary* values = item.userInfo;
    if (![values[@"version"] isEqual:@(CemuSharedRAM::ProtocolVersion)] ||
        ![values[@"nonce"] isKindOfClass:NSString.class] ||
        [values[@"nonce"] length] != 36) return;
    xpc_endpoint_t endpoint = CemuSharedRAM::UnboxEndpoint(values[@"endpoint"]);
    if (!endpoint) return;
    _connection = xpc_connection_create_from_endpoint(endpoint);
    __weak CemuSharedRAMRequestHandler* weakSelf = self;
    xpc_connection_set_event_handler(_connection, ^(xpc_object_t message) {
        @autoreleasepool {
            CemuSharedRAMRequestHandler* owner = weakSelf;
            if (!owner) return;
            if (xpc_get_type(message) == XPC_TYPE_ERROR) {
                [owner->_context completeRequestReturningItems:@[] completionHandler:nil];
                return;
            }
            [owner handleMessage:message];
        }
    });
    xpc_connection_resume(_connection);
    xpc_object_t hello = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(hello, "version", CemuSharedRAM::ProtocolVersion);
    xpc_dictionary_set_string(hello, "nonce", [values[@"nonce"] UTF8String]);
    xpc_connection_send_message(_connection, hello);
}

- (void)handleMessage:(xpc_object_t)message
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    if (!reply) return;
    xpc_dictionary_set_uint64(reply, "version", CemuSharedRAM::ProtocolVersion);
    const uint64_t operation = xpc_dictionary_get_uint64(message, "operation");
    if (xpc_dictionary_get_uint64(message, "version") == CemuSharedRAM::ProtocolVersion)
    {
        if (operation == 1 && !_backing &&
            xpc_dictionary_get_uint64(message, "bytes") == CemuSharedRAM::MEM2Bytes)
        {
            // Reserve room for the helper itself. Refuse an optimistic mapping
            // which would exceed its present allowance once MEM2 is populated.
            constexpr size_t headroom = 128u * 1024u * 1024u;
            if (os_proc_available_memory() < CemuSharedRAM::MEM2Bytes + headroom)
            {
                xpc_dictionary_set_string(reply, "reason", "insufficient helper memory allowance");
                xpc_connection_send_message(_connection, reply);
                return;
            }
            // No eager touching of 1 GiB and no executable mappings. Nonvolatile
            // storage must retain guest bytes even during memory pressure.
            if (CemuSharedRAM::CreateBacking(CemuSharedRAM::MEM2Bytes, &_backing))
            {
                _bytes = CemuSharedRAM::MEM2Bytes;
                xpc_object_t storage = xpc_shmem_create(_backing, _bytes);
                if (storage)
                {
                    CemuSharedRAM::WriteProbe(_backing, _bytes, CemuSharedRAM::ProbeInitial);
                    xpc_dictionary_set_value(reply, "storage", storage);
                    xpc_dictionary_set_uint64(reply, "bytes", _bytes);
                }
            }
        }
        else if (operation == 2 && _backing && !_verified &&
                 CemuSharedRAM::VerifyProbe(_backing, _bytes, CemuSharedRAM::ProbeResponse))
        {
            // Cross-process read-after-write handshake before publishing MEM2.
            CemuSharedRAM::WriteProbe(_backing, _bytes, 0);
            _verified = YES; // Never inspect or change guest bytes again.
            xpc_dictionary_set_bool(reply, "verified", true);
        }
    }
    xpc_connection_send_message(_connection, reply);
}

- (void)dealloc
{
    // The host mapping independently retains the VM object. Ending the helper
    // must never unmap or clear the host's live guest memory.
    if (_backing) munmap(_backing, _bytes);
}
@end
