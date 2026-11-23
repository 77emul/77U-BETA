#!/usr/bin/env python3
"""Exercise production Mach mapping and XPC protocol on macOS, not an iOS device test."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = r'''
#import <Foundation/Foundation.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include "platform/ipados/Memory/SharedRAMHost.mm"
#import <os/proc.h>
// macOS has no iOS process allowance API. Substitute only that OS reading;
// production budget checks and failure handling remain in the exercised code.
static std::atomic_size_t helperAllowance{2ull * CemuSharedRAM::MEM2Bytes};
static size_t TestHelperAllowance() { return helperAllowance.load(); }
#define os_proc_available_memory TestHelperAllowance
#include "platform/ipados/Memory/SharedRAMHelper.mm"
#undef os_proc_available_memory

@interface TestRAMContext : CemuSharedRAMContext
@property(nonatomic, strong) NSArray* testItems;
@end
@implementation TestRAMContext
- (NSArray*)inputItems { return self.testItems; }
- (void)completeRequestReturningItems:(NSArray*)items completionHandler:(void (^)(BOOL))handler {}
@end

// Replace only the OS extension launcher; all production messages, allocation,
// coherence verification, reply handling and final remapping still execute.
@interface TestRAMSession : CemuSharedRAMSession
@property(nonatomic, strong) CemuSharedRAMRequestHandler* helper;
@property(nonatomic, strong) TestRAMContext* testContext;
@end
@implementation TestRAMSession
- (void)startExtension:(NSXPCListenerEndpoint*)endpoint
{
    NSExtensionItem* item = [NSExtensionItem new];
    item.userInfo = @{@"version": @(CemuSharedRAM::ProtocolVersion), @"nonce": self.nonce, @"endpoint": endpoint};
    self.testContext = [TestRAMContext new];
    self.testContext.testItems = @[item];
    self.helper = [CemuSharedRAMRequestHandler new];
    [self.helper beginRequestWithExtensionContext:self.testContext];
}
@end

@interface SilentRAMSession : CemuSharedRAMSession
@end
@implementation SilentRAMSession
- (void)startExtension:(NSXPCListenerEndpoint*)endpoint {}
@end

static vm_prot_t Protection(void* address)
{
    mach_vm_address_t base = (mach_vm_address_t)address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    assert(mach_vm_region(mach_task_self(), &base, &size, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    if (object) mach_port_deallocate(mach_task_self(), object);
    return info.protection;
}

int main()
{
    @autoreleasepool {
        const size_t page = getpagesize();
        const size_t bytes = CemuSharedRAM::MEM2Bytes;
        assert(!CemuSharedRAM::CreateBacking(bytes, nullptr));
        CemuSharedRAM::WriteProbe(nullptr, 0, 123);
        void* reservation = mmap(nullptr, bytes + 2 * page, PROT_NONE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(reservation != MAP_FAILED);
        void* guest = (char*)reservation + page;
        assert(!CemuSharedRAM::IsAlignedRange(guest, 0, page));
        assert(!CemuSharedRAM::IsAlignedRange((char*)guest + 1, bytes, page));
        assert(!CemuSharedRAM::IsAlignedRange((void*)(UINTPTR_MAX - page + 1), 2 * page, page));
        assert(!CemuSharedRAM::MapBacking(guest, guest, page));
        assert(!CemuSharedRAM::MapBacking(guest, (char*)guest + 1, page));
        void* absent = mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(absent != MAP_FAILED);
        assert(munmap(absent, page) == 0);
        assert(!CemuSharedRAM::MapBacking(guest, absent, page));
        assert(Protection(guest) == VM_PROT_NONE);

        [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"CemuIOS.ExperimentalSharedRAM"];
        assert(!CemuSharedRAM_TryMap(guest, bytes));
        assert(!attempted.load());
        assert(Protection(guest) == VM_PROT_NONE);
        assert([CemuSharedRAMContext._allowedItemPayloadClasses containsObject:NSXPCListenerEndpoint.class]);
        assert(![CemuSharedRAMContext._allowedItemPayloadClasses containsObject:NSObject.class]);

        TestRAMSession* session = [TestRAMSession new];
        assert(![session mapAt:guest bytes:bytes]); // Main thread never blocks.
        std::atomic_bool doneState{false};
        auto* done = &doneState;
        __block bool success = false;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            success = [session mapAt:guest bytes:bytes];
            done->store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (!done->load() && std::chrono::steady_clock::now() < deadline)
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        assert(done->load() && success);
        xpc_object_t duplicate = [session requestOperation:1];
        assert(duplicate && !xpc_dictionary_get_value(duplicate, "storage"));
        CemuSharedRAM::WriteProbe(guest, bytes, CemuSharedRAM::ProbeResponse);
        xpc_object_t secondVerification = [session requestOperation:2];
        assert(secondVerification && !xpc_dictionary_get_bool(secondVerification, "verified"));
        assert(CemuSharedRAM::VerifyProbe(guest, bytes, CemuSharedRAM::ProbeResponse));
        CemuSharedRAM::WriteProbe(guest, bytes, 0);
        assert(session.storage && session.guestAddress == guest);
        assert(Protection(reservation) == VM_PROT_NONE);
        assert(Protection((char*)guest + bytes) == VM_PROT_NONE);
        assert(Protection(guest) == (VM_PROT_READ | VM_PROT_WRITE));

        // Sparse pages, both MEM2 boundaries, and three concurrent emulated-core
        // users access ordinary addresses with no RPC on loads/stores.
        assert(CemuSharedRAM::VerifyProbe(guest, bytes, 0));
        for (size_t offset = 0; offset < bytes; offset += 1024 * 1024)
            assert(((unsigned char*)guest)[offset] == 0);
        auto* counter = new ((char*)guest + page) std::atomic_uint64_t(0);
        std::vector<std::thread> cores;
        for (int core = 0; core < 3; ++core)
            cores.emplace_back([counter] { for (int i = 0; i < 100000; ++i) counter->fetch_add(1); });
        for (auto& core : cores) core.join();
        assert(counter->load() == 300000);

        // Drop transport, all XPC storage handles and the allocator's original
        // mapping: the host VM mapping must independently retain its contents.
        [session stopTransport];
        session.helper = nil;
        session.storage = nil;
        assert(counter->load() == 300000);
        CemuSharedRAM::WriteProbe(guest, bytes, 123);
        assert(CemuSharedRAM::VerifyProbe(guest, bytes, 123));
        assert(mprotect(guest, bytes, PROT_NONE) == 0);

        helperAllowance.store(bytes); // Not enough room for MEM2 plus headroom.
        TestRAMSession* lowBudget = [TestRAMSession new];
        done->store(false);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            assert(![lowBudget mapAt:guest bytes:bytes]);
            assert([lowBudget.failureReason containsString:@"insufficient"]);
            [lowBudget stopTransport];
            done->store(true);
        });
        while (!done->load()) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        assert(Protection(guest) == VM_PROT_NONE);

        // A broken/unavailable helper leaves the original reservation intact.
        SilentRAMSession* silent = [SilentRAMSession new];
        done->store(false);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            assert(![silent mapAt:guest bytes:bytes]);
            [silent stopTransport];
            done->store(true);
        });
        while (!done->load()) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        assert(Protection(guest) == VM_PROT_NONE);
        // Same fallback as the existing MemMapper reservation commit.
        assert(mprotect(guest, bytes, PROT_READ | PROT_WRITE) == 0);
        *(uint64_t*)guest = 456;
        assert(*(uint64_t*)guest == 456);
        assert(munmap(reservation, bytes + 2 * page) == 0);
        [NSUserDefaults.standardUserDefaults removeObjectForKey:@"CemuIOS.ExperimentalSharedRAM"];
        puts("PASS: production XPC handshake, 1 GiB MEM2 remap, zero fill, guards, 3-core atomics, mapping lifetime, disabled mode, insufficient budget and helper timeout fallback");
    }
}
'''

with tempfile.TemporaryDirectory(prefix='cemu-shared-ram-') as tmp:
    work = Path(tmp)
    stub = work / 'Cemu/Logging/CemuLogging.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('#pragma once\nenum class LogType { Force };\n'
                    'template<class... T> void cemuLog_log(LogType, const char*, T...) {}\n')
    test = work / 'test.mm'
    test.write_text(source)
    executable = work / 'test'
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O2', '-fobjc-arc',
                    '-I', str(work), '-I', str(root), str(test),
                    str(root / 'platform/ipados/Memory/SharedRAMMapping.mm'),
                    str(root / 'platform/ipados/Memory/SharedRAMContext.mm'),
                    '-framework', 'Foundation', '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
