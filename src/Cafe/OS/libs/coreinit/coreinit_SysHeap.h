#pragma once

#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"

namespace coreinit
{
	extern MEMHeapHandle _sysHeapHandle;

	void InitSysHeap();

	void* OSAllocFromSystem(uint32 size, uint32 alignment);
	void OSFreeToSystem(void* ptr);

	void InitializeSysHeap();
}