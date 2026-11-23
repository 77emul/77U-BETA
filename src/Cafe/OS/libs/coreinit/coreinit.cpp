#include "Cafe/OS/common/OSCommon.h"
#include "Common/SysAllocator.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"

#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"

// includes for Initialize coreinit submodules
#include "Cafe/OS/libs/coreinit/coreinit_BSP.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Atomic.h"
#include "Cafe/OS/libs/coreinit/coreinit_OverlayArena.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/OS/libs/coreinit/coreinit_GHS.h"
#include "Cafe/OS/libs/coreinit/coreinit_HWInterface.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/OS/libs/coreinit/coreinit_IM.h"
#include "Cafe/OS/libs/coreinit/coreinit_LockedCache.h"
#include "Cafe/OS/libs/coreinit/coreinit_MemoryMapping.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPCBuf.h"
#include "Cafe/OS/libs/coreinit/coreinit_Coroutine.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MCP.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_MPQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_UnitHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_FrmHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_BlockHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"

CoreinitSharedData* gCoreinitData = nullptr;

sint32 ScoreStackTrace(OSThread_t* thread, MPTR sp)
{
	uint32 stackMinAddr = thread->stackEnd.GetMPTR();
	uint32 stackMaxAddr = thread->stackBase.GetMPTR();

	sint32 score = 0;
	uint32 currentStackPtr = sp;
	for (sint32 i = 0; i < 50; i++)
	{
		uint32 nextStackPtr = memory_readU32(currentStackPtr);
		if (nextStackPtr < currentStackPtr)
			break;
		if (nextStackPtr < stackMinAddr || nextStackPtr > stackMaxAddr)
			break;
		if ((nextStackPtr & 3) != 0)
			break;
		score += 10;

		uint32 returnAddress = 0;
		returnAddress = memory_readU32(nextStackPtr + 4);
		//cemuLog_log(LogType::Force, fmt::format("SP {0:08x} ReturnAddress {1:08x}", nextStackPtr, returnAddress));
		if (returnAddress > 0 && returnAddress < 0x10000000 && (returnAddress&3) == 0)
			score += 5; // within code region
		else
			score -= 5;

		currentStackPtr = nextStackPtr;

	}
	return score;
}

void DebugLogStackTrace(OSThread_t* thread, MPTR sp)
{
	// sp might not point to a valid stackframe
	// scan stack and evaluate which sp is most likely the beginning of the stackframe

	// scan 0x400 bytes
	sint32 highestScore = -1;
	uint32 highestScoreSP = sp;
	for (sint32 i = 0; i < 0x100; i++)
	{
		uint32 sampleSP = sp + i * 4;
		sint32 score = ScoreStackTrace(thread, sampleSP);
		if (score > highestScore)
		{
			highestScore = score;
			highestScoreSP = sampleSP;
		}
	}

	cemuLog_log(LogType::Force, fmt::format("Trace starting at SP {:08x} r1={:08x}", highestScoreSP, sp));

	// print stack trace
	uint32 currentStackPtr = highestScoreSP;
	uint32 stackMinAddr = thread->stackEnd.GetMPTR();
	uint32 stackMaxAddr = thread->stackBase.GetMPTR();
	for (sint32 i = 0; i < 20; i++)
	{
		uint32 nextStackPtr = memory_readU32(currentStackPtr);
		if (nextStackPtr < currentStackPtr)
			break;
		if (nextStackPtr < stackMinAddr || nextStackPtr > stackMaxAddr)
			break;

		uint32 returnAddress = 0;
		returnAddress = memory_readU32(nextStackPtr + 4);

		RPLStoredSymbol* symbol = rplSymbolStorage_getByClosestAddress(returnAddress);

		if(symbol)
			cemuLog_log(LogType::Force, fmt::format("SP {:08x} ReturnAddr {:08x}   ({}.{}+0x{:x})", nextStackPtr, returnAddress, (const char*)symbol->libName, (const char*)symbol->symbolName, returnAddress - symbol->address));
		else
			cemuLog_log(LogType::Force, fmt::format("SP {:08x} ReturnAddr {:08x}", nextStackPtr, returnAddress));

		if (returnAddress >= 0x02000000 && returnAddress < 0x05000000)
		{
			for (uint32 a = (returnAddress >= 24) ? (returnAddress - 24) : 0; a <= returnAddress + 8; a += 4)
			{
				uint32 ins = memory_readU32(a);
				cemuLog_log(LogType::Force, fmt::format("    {:08x}: {:08x}{}", a, ins, (a == returnAddress - 4) ? " <-- CALL" : ((a == returnAddress) ? " <-- RET" : "")));
			}
		}

		currentStackPtr = nextStackPtr;
	}
}

typedef struct
{
	/* +0x00 */ uint32be name;
	/* +0x04 */ uint32be fileType; // 2 = font
	/* +0x08 */ uint32be kernelFilenamePtr;
	/* +0x0C */ MEMPTR<void> data;
	/* +0x10 */ uint32be size;
	/* +0x14 */ uint32be ukn14;
	/* +0x18 */ uint32be ukn18;
}coreinitShareddataEntry_t;

static_assert(sizeof(coreinitShareddataEntry_t) == 0x1C, "");

uint8* extractCafeDefaultFont(sint32* size);

MPTR placeholderFont = MPTR_NULL;
sint32 placeholderFontSize = 0;

void coreinitExport_OSGetSharedData(PPCInterpreter_t* hCPU)
{
	// parameters:
	// r3		sharedAreaId
	// r4		flags
	// r5		areaPtrPtr
	// r6		areaSizePtr

	// on real Wii U hw/sw there is a list of shared area entries starting at offset +0xF8000000
	// properly formated (each entry is 0x1C bytes) it looks like this:
	// FF CA FE 01 00 00 00 02 FF E8 47 AC F8 00 00 70 00 C8 0D 4C 00 00 00 00 FF FF FF FC
	// FF CA FE 02 00 00 00 02 FF E8 47 B7 F8 C8 0D C0 00 22 7E B4 00 00 00 00 00 00 11 D5
	// FF CA FE 03 00 00 00 02 FF E8 47 A0 F8 EA 8C 80 00 25 44 E0 00 00 00 00 FF A0 00 00
	// FF CA FE 04 00 00 00 02 FF E8 47 C2 F9 0F D1 60 00 7D 93 5C 00 00 00 00 FF FF FF FC

	uint32 sharedAreaId = hCPU->gpr[3];

	coreinitShareddataEntry_t* shareddataTable = (coreinitShareddataEntry_t*)memory_getPointerFromVirtualOffset(MEMORY_SHAREDDATA_AREA_ADDR);

	uint32 name = 0xFFCAFE01 + sharedAreaId;
	for (sint32 i = 0; i < 4; i++)
	{
		if ((uint32)shareddataTable[i].name == name)
		{
			memory_writeU32(hCPU->gpr[5], shareddataTable[i].data.GetMPTR());
			memory_writeU32(hCPU->gpr[6], (uint32)shareddataTable[i].size);
			osLib_returnFromFunction(hCPU, 1);
			return;
		}
	}
	// some games require a valid result or they will crash, return a pointer to our placeholder font
	cemuLog_log(LogType::Force, "OSGetSharedData() called by game but no shareddata fonts loaded. Use placeholder font");
	if (placeholderFont == MPTR_NULL)
	{
		// load and then return placeholder font
		uint8* placeholderFontPtr = extractCafeDefaultFont(&placeholderFontSize);
		placeholderFont = coreinit_allocFromSysArea(placeholderFontSize, 256);
		if (placeholderFont == MPTR_NULL)
			cemuLog_log(LogType::Force, "Failed to alloc placeholder font sys memory");
		memcpy(memory_getPointerFromVirtualOffset(placeholderFont), placeholderFontPtr, placeholderFontSize);
		free(placeholderFontPtr);
	}
	// return placeholder font
	memory_writeU32(hCPU->gpr[5], placeholderFont);
	memory_writeU32(hCPU->gpr[6], placeholderFontSize);
	osLib_returnFromFunction(hCPU, 1);
}

namespace coreinit
{
	sint32 OSGetCoreId()
	{
		return PPCInterpreter_getCoreIndex(PPCInterpreter_getCurrentInstance());
	}

	uint32 OSGetCoreCount()
	{
		return Espresso::CORE_COUNT;
	}

	uint32 OSIsDebuggerInitialized()
	{
		return 0;
	}

	uint32 OSIsDebuggerPresent()
	{
		return 0;
	}

	uint32 OSGetConsoleType()
	{
		return 0x03000050;
	}

	uint32 OSGetMainCoreId()
	{
		return 1;
	}

	bool OSIsMainCore()
	{
		return OSGetCoreId() == OSGetMainCoreId();
	}

	uint32 OSGetStackPointer()
	{
		return PPCInterpreter_getCurrentInstance()->gpr[1];
	}

	void coreinitExport_ENVGetEnvironmentVariable(PPCInterpreter_t* hCPU)
	{
		cemuLog_logDebug(LogType::Force, "ENVGetEnvironmentVariable(\"{}\",0x08x,0x{:x})", (char*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]), hCPU->gpr[4], hCPU->gpr[5]);
		char* envKeyStr = (char*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
		char* outputString = (char*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
		sint32 outputStringMaxLen = (sint32)hCPU->gpr[5];
		// also return the string "" just in case
		if (outputStringMaxLen > 0)
		{
			outputString[0] = '\0';
		}
		osLib_returnFromFunction(hCPU, 1);
	}

	bool OSIsOffBoot()
	{
		return true;
	}

	uint32 OSGetBootPMFlags()
	{
		cemuLog_logDebug(LogType::Force, "OSGetBootPMFlags() - placeholder");
		return 0;
	}

	uint32 OSGetSystemMode()
	{
		cemuLog_logDebug(LogType::Force, "OSGetSystemMode() - placeholder");
		// if this returns 2, barista softlocks shortly after boot
		return 0;
	}

	void OSPanic(const char* file, sint32 lineNumber, const char* fmt)
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		cemuLog_log(LogType::Force, "OSPanic!");
		cemuLog_log(LogType::Force, "File: {}:{}", file ? file : "null", lineNumber);
		
		if (fmt)
		{
			const char* fmtStr = (const char*)memory_getPointerFromVirtualOffsetAllowNull((uint32)hCPU->gpr[5]);
			cemuLog_log(LogType::Force, "FMT ptr (0x{:08x}): \"{}\"", (uint32)hCPU->gpr[5], fmtStr ? fmtStr : "null");
		}

		if (hCPU)
		{
			cemuLog_log(LogType::Force, "GPR[3..10]: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
				hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6],
				hCPU->gpr[7], hCPU->gpr[8], hCPU->gpr[9], hCPU->gpr[10]);
			for (int i = 3; i <= 10; ++i)
			{
				if (hCPU->gpr[i] >= 0x01000000 && hCPU->gpr[i] < 0x50000000)
				{
					const char* str = (const char*)memory_getPointerFromVirtualOffsetAllowNull(hCPU->gpr[i]);
					if (str && strlen(str) > 0 && strlen(str) < 512)
						cemuLog_log(LogType::Force, "  gpr[{}] as str: {}", i, str);
					
					uint32* words = (uint32*)memory_getPointerFromVirtualOffsetAllowNull(hCPU->gpr[i]);
					if (words)
					{
						cemuLog_log(LogType::Force, "  gpr[{}] words: {:08x} {:08x} {:08x} {:08x}",
							i, _swapEndianU32(words[0]), _swapEndianU32(words[1]), _swapEndianU32(words[2]), _swapEndianU32(words[3]));
					}
				}
			}
		}

		DebugLogStackTrace(coreinit::OSGetCurrentThread(), coreinit::OSGetStackPointer());
	}

	void InitializeCore()
	{
		cafeExportRegister("coreinit", OSGetCoreId, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetCoreCount, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSIsDebuggerInitialized, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSIsDebuggerPresent, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetConsoleType, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetMainCoreId, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSIsMainCore, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetStackPointer, LogType::CoreinitThread);

		osLib_addFunction("coreinit", "ENVGetEnvironmentVariable", coreinitExport_ENVGetEnvironmentVariable);

		cafeExportRegister("coreinit", OSIsOffBoot, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetBootPMFlags, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMode, LogType::CoreinitThread);

		cafeExportRegister("coreinit", OSPanic, LogType::Placeholder);
	}

	void InitializeSharedData()
	{
		if (!gCoreinitData)
			gCoreinitData = (CoreinitSharedData*)memory_getPointerFromVirtualOffset(coreinit_allocFromSysArea(sizeof(CoreinitSharedData), 32));

		// CEMU_AREA survives foreground-title switches, but these weak-link slots
		// belong to the current Cafe OS process.  Leaving the previous title's
		// function pointers here can send the next title into stale guest code.
		memset(gCoreinitData, 0x00, sizeof(CoreinitSharedData));

		osLib_addVirtualPointer("coreinit", "MEMAllocFromDefaultHeap", memory_getVirtualOffsetFromPointer(&gCoreinitData->MEMAllocFromDefaultHeap));
		osLib_addVirtualPointer("coreinit", "MEMAllocFromDefaultHeapEx", memory_getVirtualOffsetFromPointer(&gCoreinitData->MEMAllocFromDefaultHeapEx));
		osLib_addVirtualPointer("coreinit", "MEMFreeToDefaultHeap", memory_getVirtualOffsetFromPointer(&gCoreinitData->MEMFreeToDefaultHeap));
		osLib_addVirtualPointer("coreinit", "__atexit_cleanup", memory_getVirtualOffsetFromPointer(&gCoreinitData->__atexit_cleanup));
		osLib_addVirtualPointer("coreinit", "__stdio_cleanup", memory_getVirtualOffsetFromPointer(&gCoreinitData->__stdio_cleanup));
		osLib_addVirtualPointer("coreinit", "__cpp_exception_cleanup_ptr", memory_getVirtualOffsetFromPointer(&gCoreinitData->__cpp_exception_cleanup_ptr));
		osLib_addVirtualPointer("coreinit", "__cpp_exception_init_ptr", memory_getVirtualOffsetFromPointer(&gCoreinitData->__cpp_exception_init_ptr));
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "coreinit";
		}

		void RPLMapped() override
		{
			coreinit::InitializeCore();
			coreinit::InitializeSchedulerLock();
			coreinit::InitializeSysHeap();

			// allocate coreinit global data & weak links
			coreinit::InitializeSharedData();

			// init GHS and threads
			coreinit::PrepareGHSRuntime();
			coreinit::MapThreadExports();

			// reset threads
			activeThreadCount = 0;
			// init submodules
			coreinit::InitializeMEM();
			coreinit::InitializeMEMFrmHeap();
			coreinit::InitializeMEMUnitHeap();
			coreinit::InitializeMEMBlockHeap();
			coreinit::InitializeFG();
			coreinit::InitializeBSP();
			coreinit::InitializeMCP();
			coreinit::InitializeOverlayArena();
			coreinit::InitializeDynLoad();
			coreinit::InitializeGHS();
			coreinit::InitializeHWInterface();
			coreinit::InitializeAtomic();
			coreinit::InitializeMemory();
			coreinit::InitializeIM();
			coreinit::InitializeLC();
			coreinit::InitializeMP();
			coreinit::InitializeTimeAndCalendar();
			coreinit::MapAlarmExports();
			coreinit::InitializeFS();
			coreinit::InitializeSystemInfo();
			coreinit::InitializeConcurrency();
			coreinit::InitializeSpinlock();
			coreinit::InitializeMessageQueue();
			coreinit::MapIPCExports();
			coreinit::InitializeIPCBuf();
			coreinit::InitializeMemoryMapping();
			coreinit::InitializeCodeGen();
			coreinit::InitializeCoroutine();
			coreinit::InitializeOSScreen();

			// legacy mem stuff
			coreinit::expheap_load();

			// misc exports
			coreinit::miscInit();
			osLib_addFunction("coreinit", "OSGetSharedData", coreinitExport_OSGetSharedData);
			osLib_addFunction("coreinit", "UCReadSysConfig", coreinitExport_UCReadSysConfig);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				coreinit::InitializeThread();
				coreinit::InitializeAlarm();
				coreinit::InitializeIPC();
				InitializeAsyncCallback();
				// remaining coreinit initialization happens in coreinit_start and requires a valid PPC context
				OSThread_t* initialThread = coreinit::OSGetDefaultThread(1);
				coreinit::OSSetThreadPriority(initialThread, 16);
				coreinit::OSRunThread(initialThread, PPCInterpreter_makeCallableExportDepr(coreinit_start), 0, nullptr);
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				// todo
			}
		}
	}s_COSCoreinitModule;

	COSModule* GetModule()
	{
		return &s_COSCoreinitModule;
	}
}
