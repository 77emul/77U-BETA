#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysArea.h"
#include "Cafe/HW/MMU/MMU.h"

// the system area is a block of memory that exists only in the emulator. It is used to simplify dynamic memory allocation for system data
// this partially overlaps with the system heap from coreinit_SysHeap.cpp -> Use SysHeap for everything
MPTR sysAreaAllocatorOffset = 0;

MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment)
{
	cemu_assert_debug(mmuRange_CEMU_AREA.isMapped());
	// deprecated, use OSAllocFromSystem instead (for everything but SysAllocator which probably should use its own allocator?)
	static std::mutex s_allocator_mutex;
	s_allocator_mutex.lock();
	sysAreaAllocatorOffset = (sysAreaAllocatorOffset+alignment-1);
	sysAreaAllocatorOffset -= (sysAreaAllocatorOffset%alignment);
	uint32 newMemOffset =  mmuRange_CEMU_AREA.getBase() + sysAreaAllocatorOffset;
	sysAreaAllocatorOffset += (size+3)&~3;
	if( sysAreaAllocatorOffset >= mmuRange_CEMU_AREA.getSize() )
	{
		cemuLog_log(LogType::Force, "Ran out of system memory");
		cemu_assert(false); // out of bounds
	}
	s_allocator_mutex.unlock();
	return newMemOffset;
}

void coreinit_freeToSysArea(MPTR mem)
{
	// todo
}
