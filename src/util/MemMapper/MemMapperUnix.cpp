#include "util/MemMapper/MemMapper.h"

#include <unistd.h>
#include <sys/mman.h>
#include <cstdint>

namespace MemMapper
{
	const size_t sPageSize{ []()
		{
			long ps = sysconf(_SC_PAGESIZE);
			if (ps <= 0)
				ps = (long)getpagesize();
			if (ps <= 0)
				return (size_t)0;
			return (size_t)ps;
		}()
	};

	size_t GetPageSize()
	{
		return sPageSize;
	}

	int GetProt(PAGE_PERMISSION permissionFlags)
	{
		int p = 0;
		if (permissionFlags == PAGE_PERMISSION::P_NONE)
			p = PROT_NONE;
		else if ((permissionFlags & PAGE_PERMISSION::P_READ) != 0 && (permissionFlags & PAGE_PERMISSION::P_WRITE) != 0 && (permissionFlags & PAGE_PERMISSION::P_EXECUTE) != 0)
			p = PROT_READ | PROT_WRITE | PROT_EXEC;
		else if ((permissionFlags & PAGE_PERMISSION::P_READ) != 0 && (permissionFlags & PAGE_PERMISSION::P_WRITE) != 0 && (permissionFlags & PAGE_PERMISSION::P_EXECUTE) == 0)
			p = PROT_READ | PROT_WRITE;
		else if ((permissionFlags & PAGE_PERMISSION::P_READ) != 0 && (permissionFlags & PAGE_PERMISSION::P_WRITE) == 0 && (permissionFlags & PAGE_PERMISSION::P_EXECUTE) == 0)
			p = PROT_READ;
		else
			p = 0;
		return p;
	}

	static bool GetPageSpan(void* baseAddr, size_t size, size_t pageSize, void** outPageStart, size_t* outAlignedSize)
	{
		if (!baseAddr || size == 0 || pageSize == 0)
			return false;

		uintptr_t start_addr = (uintptr_t)baseAddr;
		// Check for overflow on start_addr + size
		if (UINTPTR_MAX - start_addr < size)
			return false;

		uintptr_t end_addr = start_addr + size;
		uintptr_t page_start = (start_addr / pageSize) * pageSize;

		uintptr_t page_end;
		uintptr_t remainder = end_addr % pageSize;
		if (remainder == 0)
		{
			page_end = end_addr;
		}
		else
		{
			uintptr_t round_up = pageSize - remainder;
			if (UINTPTR_MAX - end_addr < round_up)
				return false; // overflow
			page_end = end_addr + round_up;
		}

		if (page_end < page_start)
			return false;

		uintptr_t span = page_end - page_start;
		if (span > SIZE_MAX)
			return false;

		*outPageStart = (void*)page_start;
		*outAlignedSize = (size_t)span;
		return true;
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		void* r = mmap(baseAddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (r == MAP_FAILED)
			return nullptr;
		return r;
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		munmap(baseAddr, size);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		void* r = nullptr;
		if (fromReservation)
		{
			size_t pageSize = GetPageSize();
			void* pageStart = nullptr;
			size_t alignedSize = 0;
			if (!GetPageSpan(baseAddr, size, pageSize, &pageStart, &alignedSize))
				return nullptr;

			if (mprotect(pageStart, alignedSize, GetProt(permissionFlags)) == 0)
				r = baseAddr;
			else
				r = nullptr;
		}
		else
		{
			r = mmap(baseAddr, size, GetProt(permissionFlags), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (r == MAP_FAILED)
				r = nullptr;
		}
		return r;
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if (fromReservation)
		{
			size_t pageSize = GetPageSize();
			void* pageStart = nullptr;
			size_t alignedSize = 0;
			if (!GetPageSpan(baseAddr, size, pageSize, &pageStart, &alignedSize))
				return;

			mprotect(pageStart, alignedSize, PROT_NONE);
		}
		else
		{
			munmap(baseAddr, size);
		}
	}

};
