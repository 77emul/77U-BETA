#pragma once

#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Common/SysAllocator.h"

namespace coreinit
{
	void MapThreadStateExports();
	void __OSInitDefaultThreads();
	void __OSUpdateThreadEffectivePriority(OSThread_t* thread);
	void __OSSetThreadBasePriority(OSThread_t* thread, sint32 newPriority);
	void __OSInitContext(OSContext_t* ctx, MEMPTR<void> initialIP, MEMPTR<void> initialStackPointer);
	void __OSThreadInit(OSThread_t* thread, MEMPTR<void> entrypoint, uint32 argInt, MEMPTR<void> argPtr, MEMPTR<void> stackTop, uint32 stackSize, sint32 priority, uint32 upirCoreIndex, OSThread_t::THREAD_TYPE threadType);
	bool __OSCreateThreadInternal2(OSThread_t* thread, MEMPTR<void> entrypoint, uint32 argInt, MEMPTR<void> argPtr, MEMPTR<void> stackBase, uint32 stackSize, sint32 priority, uint32 attrBits, OSThread_t::THREAD_TYPE threadType);

	extern SysAllocator<OSThreadQueue> g_activeThreadQueue;
	extern SysAllocator<OSThreadQueue, 3> g_coreRunQueue;
	extern SysAllocator<OSThread_t, PPC_CORE_COUNT> s_defaultThreads;
	extern SysAllocator<uint8, PPC_CORE_COUNT * 1024 * 1024> s_stack;
	extern SysAllocator<char, 64> _defaultThreadName[PPC_CORE_COUNT];
	extern bool g_isMulticoreMode;
}
