#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"

PPCCodeHeap rplLoaderHeap_lowerAreaCodeMem2(nullptr, MEMORY_CODE_TRAMPOLINE_AREA_SIZE);
ChunkedFlatAllocator<64 * 1024> g_heapTrampolineArea;
uint32 rplLoader_maxCodeAddress = 0;
uint32 rplLoader_sdataAddr = 0;
uint32 rplLoader_sdata2Addr = 0;
uint32 rplLoader_currentDataAllocatorAddr = 0x10000000;
std::map<void(*)(PPCInterpreter_t* hCPU), uint32> g_map_callableExports;

uint32 RPLLoader_GetSDA1Base()
{
	return rplLoader_sdataAddr;
}

uint32 RPLLoader_GetSDA2Base()
{
	return rplLoader_sdata2Addr;
}

uint32 RPLLoader_GetDataAllocatorAddr()
{
	return (rplLoader_currentDataAllocatorAddr + 0xFFF) & (~0xFFF);
}

struct mappedFunctionImport_t
{
	uint64 hash1;
	uint64 hash2;
	uint32 address;
};
std::vector<mappedFunctionImport_t> list_mappedFunctionImports;

void RPLLoader_InitTrampolineHeap()
{
	if (!mmuRange_TRAMPOLINE_AREA.isMapped())
		mmuRange_TRAMPOLINE_AREA.mapMem();
	if (rplLoaderHeap_lowerAreaCodeMem2.getHeapBase() != nullptr)
		return;
	cemu_assert_debug(!rplLoaderHeap_lowerAreaCodeMem2.hasAllocations());
	rplLoaderHeap_lowerAreaCodeMem2.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_CODE_TRAMPOLINE_AREA_ADDR));
	g_heapTrampolineArea.setBaseAllocator(&rplLoaderHeap_lowerAreaCodeMem2);
}

void RPLLoader_ReleaseTrampolineHeap()
{
	g_heapTrampolineArea.releaseAll();
	list_mappedFunctionImports.clear();
	g_map_callableExports.clear();
}

uint8* RPLLoader_AllocateTrampolineCodeSpace(RPLModule* rplLoaderContext, sint32 size)
{	
	if (rplLoaderContext)
	{
		// allocation owned by rpl
		return (uint8*)rplLoaderContext->heapTrampolineArea.alloc(size, 4);
	}
	// allocation owned by global context
	auto result = (uint8*)g_heapTrampolineArea.alloc(size, 4);
	rplLoader_maxCodeAddress = std::max(rplLoader_maxCodeAddress, memory_getVirtualOffsetFromPointer(g_heapTrampolineArea.getCurrentBlockPtr()) + g_heapTrampolineArea.getCurrentBlockOffset());
	return result;
}

uint8* RPLLoader_AllocateTrampolineCodeSpace(sint32 size)
{
	return RPLLoader_AllocateTrampolineCodeSpace(nullptr, size);
}

uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(PPCInterpreter_t* hCPU))
{
	auto it = g_map_callableExports.find(ppcCallableExport);
	if (it != g_map_callableExports.end())
		return it->second;
	// get HLE function index
	sint32 functionIndex = PPCInterpreter_registerHLECall(ppcCallableExport, fmt::format("PPCCallback{:x}", (uintptr_t)ppcCallableExport));
	MPTR codeAddr = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(4));
	uint32 opcode = (1 << 26) | functionIndex;
	memory_write<uint32>(codeAddr, opcode);
	g_map_callableExports[ppcCallableExport] = codeAddr;
	return codeAddr;
}

void _calculateMappedImportNameHash(const char* rplName, const char* funcName, uint64* h1Out, uint64* h2Out)
{
	uint64 h1 = 0x1111111111111111;
	uint64 h2 = 0x2222222222222222;
	// rplName
	while (*rplName)
	{
		uint64 v = (uint64)*rplName;
		h1 += v;
		h2 ^= v;
		h1 = std::rotl<uint64>(h1, 3);
		h2 = std::rotl<uint64>(h2, 7);
		rplName++;
	}
	// funcName
	while (*funcName)
	{
		uint64 v = (uint64)*funcName;
		h1 += v;
		h2 ^= v;
		h1 = std::rotl<uint64>(h1, 3);
		h2 = std::rotl<uint64>(h2, 7);
		funcName++;
	}
	*h1Out = h1;
	*h2Out = h2;
}

uint32 rpl_mapHLEImport(RPLModule* rplLoaderContext, const char* rplName, const char* funcName, bool functionMustExist)
{
	// calculate import name hash
	uint64 mappedImportHash1;
	uint64 mappedImportHash2;
	_calculateMappedImportNameHash(rplName, funcName, &mappedImportHash1, &mappedImportHash2);
	// find already mapped name
	for (auto& importItr : list_mappedFunctionImports)
	{
		if (importItr.hash1 == mappedImportHash1 && importItr.hash2 == mappedImportHash2)
		{
			return importItr.address;
		}
	}
	// copy lib file name and cut off .rpl from libName if present
	char libName[512];
	strcpy_s(libName, rplName);
	for (sint32 i = 0; i < sizeof(libName); i++)
	{
		if (libName[i] == '\0')
			break;
		if (libName[i] == '.')
		{
			libName[i] = '\0';
			break;
		}
	}
	// find import in list of known exports
	sint32 functionIndex = osLib_getFunctionIndex(libName, funcName);
	if (functionIndex >= 0)
	{
		MPTR codeAddr = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(4));
		uint32 opcode = (1 << 26) | functionIndex;
		memory_write<uint32>(codeAddr, opcode);
		// register mapped import
		mappedFunctionImport_t newImport;
		newImport.hash1 = mappedImportHash1;
		newImport.hash2 = mappedImportHash2;
		newImport.address = codeAddr;
		list_mappedFunctionImports.push_back(newImport);
		// remember in symbol storage for debugger
		rplSymbolStorage_store(libName, funcName, codeAddr);
		return codeAddr;
	}
	else
	{
		if (functionMustExist == false)
			return MPTR_NULL;
	}
	// create code for unsupported import
	uint32 codeStart = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(256));
	uint32 currentAddress = codeStart;
	uint32 opcode = (1 << 26) | (0xFFD0); // opcode for HLE: Unsupported import
	memory_write<uint32>(codeStart + 0, opcode);
	memory_write<uint32>(codeStart + 4, 0x4E800020);
	currentAddress += 8;
	// write name of lib/function
	sint32 libNameLength = std::min(128, (sint32)strlen(libName));
	sint32 funcNameLength = std::min(128, (sint32)strlen(funcName));
	memcpy(memory_getPointerFromVirtualOffset(currentAddress), libName, libNameLength);
	currentAddress += libNameLength;
	memory_writeU8(currentAddress, '.');
	currentAddress++;
	memcpy(memory_getPointerFromVirtualOffset(currentAddress), funcName, funcNameLength);
	currentAddress += funcNameLength;
	memory_writeU8(currentAddress, '\0');
	currentAddress++;
	// align address to 4 byte boundary
	currentAddress = (currentAddress + 3)&~3;
	// register mapped import
	mappedFunctionImport_t newImport;
	newImport.hash1 = mappedImportHash1;
	newImport.hash2 = mappedImportHash2;
	newImport.address = codeStart;
	list_mappedFunctionImports.push_back(newImport);
	// remember in symbol storage for debugger
	rplSymbolStorage_store(libName, funcName, codeStart);
	// return address of code start
	return codeStart;
}
