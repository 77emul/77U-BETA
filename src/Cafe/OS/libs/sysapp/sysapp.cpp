#include "Cafe/OS/common/OSCommon.h"
#include "sysapp.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"

typedef struct  
{
	MEMPTR<char> argStr;
	uint32be size;
}sysStandardArguments_t;

typedef struct
{
	// guessed
	/* +0x00 */ MEMPTR<uint8>	sysAnchor;
	/* +0x04 */ uint32be		sysAnchorSize;
	/* +0x08 */ MEMPTR<uint8>	sysResult;
	/* +0x0C */ uint32be		sysResultSize;
}sysDeserializeStandardArguments_t;

typedef struct
{
	// used for setting arguments, maybe wrong?
	/* +0x00 */ sysStandardArguments_t standardArguments;
	/* +0x08 */ uint32be mode;
	/* +0x0C */ uint32be slot_id;
	/* +0x10 */ MEMPTR<uint8> mii;
}sysMiiStudioArguments_t;

typedef struct  
{
	// used for getting arguments
	/* +0x00 */ sysStandardArguments_t standardArguments; // ?
	/* +0x08 */ uint32 ukn08;
	/* +0x0C */ uint32 ukn0C;
	/* +0x10 */ uint32be mode;
	/* +0x14 */ uint32be slot_id;
	/* +0x18 */ MEMPTR<uint8> mii;
	/* +0x1C */ uint32be miiSize;
}sysMiiStudioArguments2_t;

typedef struct  
{
	/* +0x00 */ sysDeserializeStandardArguments_t standardArguments;
	/* +0x10 */ uint32be jumpTo; // see below for list of available values
	/* +0x14 */ uint32be needsReturn;
	/* +0x18 */ uint32be firstBootMode;
}sysSettingsArguments_t;

static_assert(sizeof(sysSettingsArguments_t) == 0x1C);
static_assert(offsetof(sysSettingsArguments_t, jumpTo) == 0x10);
static_assert(offsetof(sysSettingsArguments_t, needsReturn) == 0x14);
static_assert(offsetof(sysSettingsArguments_t, firstBootMode) == 0x18);

/*
Sys settings jumpTo values:
0x00 -> Normal launch
0x01 -> Internet settings
0x02 -> Data management
0x03 -> TV remote
0x04 -> Date settings
0x05 -> Country settings
0x06 -> System update

0x64 -> Initial system configuration (after Wii U GamePad was configured)
0x65 -> Crashes
0x66 -> Delete All Content and Settings (throws erreula after a second)
0x67 -> Quick Start Settings
0x68 -> TV connection type
0x69 -> Data management
0xFF -> Data transfer
*/

typedef struct
{
	/* +0x00 */ sysDeserializeStandardArguments_t standardArguments; // guessed
	/* +0x10 */ uint32be ukn10;
	/* +0x14 */ uint32be ukn14;
}eshopArguments_t;

static_assert(sizeof(eshopArguments_t) == 0x18);

#define SYS_STANDARD_ARGS_MAX_LEN		0x1000

#define OS_COPY_MAX_DATA_SIZE			(0x400000-4)

void appendDataToBuffer(uint32 currentCopyDataSize, char* str, sint32 size)
{
	cemu_assert_unimplemented();
}

sint32 _serializeArgsToBuffer(uint32 currentCopyDataSize, const char* argName, char* str, sint32 size, void (*appendFunc)(uint32 currentCopyDataSize, char* str, sint32 size))
{
	if (strnlen(argName, 0x41) == 0x40)
		return -0x2710;
	if (size > 0x200000)
		return -0x7530;
	char tempStr[0x80];
	if (size != 0 && str != nullptr)
	{
		sint32 textSize = (sint32)snprintf(tempStr, sizeof(tempStr), "<%s %s=%u>", argName, "size", size);
		if ((currentCopyDataSize + textSize + size) >= OS_COPY_MAX_DATA_SIZE)
			return 0xFFFF63C0;
		appendFunc(currentCopyDataSize, str, size);
		appendFunc(currentCopyDataSize, tempStr, textSize);
	}
	else
	{
		sint32 textSize = (sint32)snprintf(tempStr, sizeof(tempStr), "<%s>", argName);
		appendFunc(currentCopyDataSize, tempStr, textSize);
	}
	return 0;
}

sint32 SYSPackArgs()
{
	uint32 currentCopyDataSize = coreinit::__OSGetCopyDataSize();
	char sysPackStr[128];
	sint32 textSize = (sint32)snprintf(sysPackStr, sizeof(sysPackStr), "<%s %s=%u>", "sys:pack", "size", currentCopyDataSize);
	if ((currentCopyDataSize + textSize) > OS_COPY_MAX_DATA_SIZE)
		return 0xFFFF4480;
	coreinit::__OSAppendCopyData((uint8*)sysPackStr, textSize);
	return 0;
}

sint32 SYSSerializeSysArgs(const char* argName, char* str, sint32 size)
{
	uint32 currentCopyDataSize = coreinit::__OSGetCopyDataSize();
	sint32 r = _serializeArgsToBuffer(currentCopyDataSize, argName, str, size, appendDataToBuffer);
	if (r < 0)
		return r;
	return 0;
}

sint32 _SYSSerializeStandardArgsIn(sysStandardArguments_t* standardArgs)
{
	if (strnlen(standardArgs->argStr.GetPtr(), SYS_STANDARD_ARGS_MAX_LEN + 4) > SYS_STANDARD_ARGS_MAX_LEN)
	{
		return 0xFFFF63C0;
	}
	SYSSerializeSysArgs("sys:anchor", standardArgs->argStr.GetPtr(), standardArgs->size);
	SYSPackArgs();
	return 0;
}

sint32 _SYSAppendCallerInfo()
{
	// todo
	return 0;
}

typedef struct  
{
	uint32be id;
	uint32be type;
	// union data here - todo
}sysArgSlot_t;

#define SYS_ARG_ID_END				0	// indicates end of sysArgSlot_t array
#define SYS_ARG_ID_ANCHOR			100

typedef struct
{
	char* argument;
	uint32 size;
	uint8* data;
}deserializedArg_t;

uint32 _sysArg_packSize = 0;

void clearSysArgs()
{
	_sysArg_packSize = 0;
}

void deserializeSysArg(deserializedArg_t* deserializedArg)
{
	if (strcmp(deserializedArg->argument, "sys:pack") == 0)
		_sysArg_packSize = deserializedArg->size;
	// todo
}

sint32 _deserializeSysArgsEx2(uint8* copyDataPtr, sint32 copyDataSize, void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam), void* customParam)
{
	sint32 idx = copyDataSize - 1;
	sint32 argSlotIndex = 0;
	while (idx >= 0)
	{
		if (copyDataPtr[idx] == '>')
		{
			// find matching '<'
			sint32 idxStart = idx;
			while (true)
			{
				idxStart--;
				if (idxStart < 0)
					return 1;
				if (copyDataPtr[idxStart] == '<')
					break;
			}
			sint32 idxDataEnd = idxStart;
			idxStart++;
			// parse argument name
			char argumentName[64];
			sint32 idxCurrent = idxStart;
			while( idxCurrent <= idx )
			{
				argumentName[idxCurrent - idxStart] = copyDataPtr[idxCurrent];
				if ((idxCurrent - idxStart) >= 60)
					return 1;
				if (copyDataPtr[idxCurrent] == ' ' || copyDataPtr[idxCurrent] == '>')
				{
					argumentName[idxCurrent - idxStart] = '\0';
					break;
				}
				idxCurrent++;
			}
			// parse size (if any)
			while (copyDataPtr[idxCurrent] == ' ')
				idxCurrent++;
			sint32 argumentDataSize;
			if (copyDataPtr[idxCurrent] == '>')
			{
				argumentDataSize = 0;
			}
			else if ((copyDataSize - idxCurrent) >= 5 && memcmp(copyDataPtr+idxCurrent, "size", 4) == 0)
			{
				idxCurrent += 4;
				// skip whitespaces and '='
				while (copyDataPtr[idxCurrent] == ' ' || copyDataPtr[idxCurrent] == '=')
					idxCurrent++;
				// parse size value
				char tempSizeStr[32];
				sint32 sizeParamLen = idx - idxCurrent;
				if (sizeParamLen < 0 || sizeParamLen >= 30)
					return 1;
				memcpy(tempSizeStr, copyDataPtr+idxCurrent, sizeParamLen);
				tempSizeStr[sizeParamLen] = '\0';
				argumentDataSize = (sint32)atol(tempSizeStr);
			}
			else
			{
				assert_dbg();
				return 1;
			}
			idx = idxStart - argumentDataSize - 1; // beginning of data
			deserializedArg_t deserializedArg = { 0 };
			deserializedArg.argument = argumentName;
			deserializedArg.size = argumentDataSize;
			deserializedArg.data = copyDataPtr + idx;
			deserializeSysArg(&deserializedArg);
			if (cbDeserializeArg)
				cbDeserializeArg(&deserializedArg, customParam);

			idx--;
		}
		else if (copyDataPtr[idx] == ']')
		{
			assert_dbg();
		}
		else
			return 1;
	}
	return 0;
}

void _deserializeSysArgsEx(void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam), void* customParam)
{
	sint32 copyDataSize = coreinit::__OSGetCopyDataSize();
	uint8* copyDataPtr = coreinit::__OSGetCopyDataPtr();
	_deserializeSysArgsEx2(copyDataPtr, copyDataSize, cbDeserializeArg, customParam);
}

void SYSDeserializeSysArgs(void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam), sysArgSlot_t* argSlots)
{
	_deserializeSysArgsEx(cbDeserializeArg, argSlots);
}

sint32 _deserializeArgs(void* customParam, sint32 customParamSize, void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam))
{
	memset(customParam, 0, customParamSize);
	clearSysArgs();
	_deserializeSysArgsEx(cbDeserializeArg, customParam);
	return 0;
}

void _unpackSysWorkaround()
{
	// unpack sys
	clearSysArgs();
	_deserializeSysArgsEx(NULL, NULL);
	if (_sysArg_packSize != 0)
	{
		coreinit::__OSResizeCopyData(_sysArg_packSize);
	}
}

void cbDeserializeArg_MiiMaker(deserializedArg_t* deserializedArg, void* customParam)
{
	sysMiiStudioArguments2_t* miiStudioArgs = (sysMiiStudioArguments2_t*)customParam;

	if (strcmp(deserializedArg->argument, "mode") == 0)
	{
		miiStudioArgs->mode = *(uint32be*)deserializedArg->data;
	}
	else if (strcmp(deserializedArg->argument, "mii") == 0)
	{
		assert_dbg();
	}
	else if (strcmp(deserializedArg->argument, "slot_id") == 0)
	{
#ifdef CEMU_DEBUG_ASSERT
		assert_dbg();
#endif
	}
	else
	{
#ifdef CEMU_DEBUG_ASSERT
		assert_dbg();
#endif
	}
}

sint32 _SYSGetMiiStudioArgs(sysMiiStudioArguments2_t* miiStudioArgs)
{
	_unpackSysWorkaround();
	_deserializeArgs(miiStudioArgs, sizeof(sysMiiStudioArguments2_t), cbDeserializeArg_MiiMaker);
	return 0;
}

void sysappExport__SYSGetMiiStudioArgs(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(miiStudioArgs, sysMiiStudioArguments2_t, 0);
	sint32 r = _SYSGetMiiStudioArgs(miiStudioArgs);
	osLib_returnFromFunction(hCPU, r);
}

void cbDeserializeArg_SysSettings(deserializedArg_t* deserializedArg, void* customParam)
{
	sysSettingsArguments_t* settingsArgs = (sysSettingsArguments_t*)customParam;
	if (boost::iequals(deserializedArg->argument, "jump_to"))
	{
		cemu_assert_unimplemented();
	}
	else if (boost::iequals(deserializedArg->argument, "first_boot_kind"))
	{
		cemu_assert_unimplemented();
	}
	else if (boost::iequals(deserializedArg->argument, "needs_return"))
	{
		settingsArgs->needsReturn = 1;
	}
	else
		cemu_assert_unimplemented();
}

sint32 _SYSGetSettingsArgs(sysSettingsArguments_t* settingsArgs)
{
	_unpackSysWorkaround();
	memset(settingsArgs, 0, sizeof(sysSettingsArguments_t));
	return _deserializeArgs(settingsArgs, sizeof(sysSettingsArguments_t), cbDeserializeArg_SysSettings);
}

void sysappExport__SYSGetSettingsArgs(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(settingsArgs, sysSettingsArguments_t, 0);
	sint32 r = _SYSGetSettingsArgs(settingsArgs);

	if (settingsArgs->jumpTo == 0) // workaround when launching sys settings directly
		settingsArgs->jumpTo = 0x00;

	osLib_returnFromFunction(hCPU, r);
}

struct  
{
	uint64 t0;
	uint64 t1;
	uint64 t2;
}systemApplicationTitleId[] = {
	{ 0x0005001010040000ULL, 0x0005001010040100ULL, 0x0005001010040200ULL }, // Wii U Menu 
	{ 0x0005001010047000ULL, 0x0005001010047100ULL, 0x0005001010047200ULL }, // System Settings 
	{ 0x0005001010048000ULL, 0x0005001010048100ULL, 0x0005001010048200ULL }, // Parental Controls
	{ 0x0005001010049000ULL, 0x0005001010049100ULL, 0x0005001010049200ULL }, // User Settings 
	{ 0x000500101004A000ULL, 0x000500101004A100ULL, 0x000500101004A200ULL }, // Mii Maker 
	{ 0x000500101004B000ULL, 0x000500101004B100ULL, 0x000500101004B200ULL }, // Account Settings
	{ 0x000500101004C000ULL, 0x000500101004C100ULL, 0x000500101004C200ULL }, // Daily Log 
	{ 0x000500101004D000ULL, 0x000500101004D100ULL, 0x000500101004D200ULL }, // Notifications
	{ 0x000500101004E000ULL, 0x000500101004E100ULL, 0x000500101004E200ULL }, // Health and Safety Information 
	{ 0x0005001B10059000ULL, 0x0005001B10059100ULL, 0x0005001B10059200ULL }, // Wii U Electronic Manual 
	{ 0x000500101005A000ULL, 0x000500101005A100ULL, 0x000500101005A200ULL }, // Wii U Chat 
	{ 0x0005001010062000ULL, 0x0005001010062100ULL, 0x0005001010062200ULL }  // Software/Data Transfer 
};

uint64 _SYSGetSystemApplicationTitleIdByProdArea(uint32 systemApplicationId, uint32 platformRegion)
{
	if (systemApplicationId >= (sizeof(systemApplicationTitleId) / sizeof(systemApplicationTitleId[0])) )
		return 0;

	if (platformRegion == 1)
	{
		return systemApplicationTitleId[systemApplicationId].t0;
	}
	else if (platformRegion == 4 || platformRegion == 8)
	{
		return systemApplicationTitleId[systemApplicationId].t2;
	}
	return systemApplicationTitleId[systemApplicationId].t1;
}

uint64 _SYSGetSystemApplicationTitleId(sint32 index)
{
	uint32 region = (uint32)CafeSystem::GetPlatformRegion();
	uint64 titleId = _SYSGetSystemApplicationTitleIdByProdArea(index, region);
	cemuLog_log(LogType::Force, "_SYSGetSystemApplicationTitleId({}) -> 0x{:016x} (region={})", index, titleId, region);
	return titleId;
}

void __LaunchMiiMaker(sysMiiStudioArguments_t* args, uint32 platformRegion)
{
	coreinit::__OSClearCopyData();
	if (args)
	{
		_SYSSerializeStandardArgsIn(&args->standardArguments);
		SYSSerializeSysArgs("mode", (char*)&args->mode, 4);
		SYSSerializeSysArgs("slot_id", (char*)&args->slot_id, 4);
		if(args->mii)
			SYSSerializeSysArgs("mii", (char*)args->mii.GetPtr(), 0x60);
	}
	_SYSAppendCallerInfo();
	uint64 titleIdToLaunch = _SYSGetSystemApplicationTitleIdByProdArea(4, platformRegion);
	cemuLog_log(LogType::Force, "LaunchMiiMaker called (titleId: {:016x}). Applet launch not supported", titleIdToLaunch);
	coreinit::StartBackgroundForegroundTransition();
}

void _SYSLaunchMiiStudio(sysMiiStudioArguments_t* args)
{
	__LaunchMiiMaker(args, (uint32)CafeSystem::GetPlatformRegion());
}

void sysappExport__SYSLaunchMiiStudio(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(args, sysMiiStudioArguments_t, 0);
	_SYSLaunchMiiStudio(args);
}

void sysappExport__SYSReturnToCallerWithStandardResult(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32BEPtr(resultPtr, 0);
	cemuLog_log(LogType::Force, "_SYSReturnToCallerWithStandardResult(0x{:08x}) result: 0x{:08x}", hCPU->gpr[3], (uint32)*resultPtr);
	while (true) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void sysappExport__SYSGetEShopArgs(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(args, eshopArguments_t, 0);
	cemuLog_logDebug(LogType::Force, "_SYSGetEShopArgs() - placeholder");
	memset(args, 0, sizeof(eshopArguments_t));

	osLib_returnFromFunction(hCPU, 0);
}

sint32 SYSGetUPIDFromTitleID(uint64 titleId)
{
	uint32 titleIdHigh = (titleId >> 32);
	uint32 titleIdLow = (uint32)(titleId & 0xFFFFFFFF);
	if ((titleIdHigh & 0xFFFF0000) != 0x50000)
	{
		return -1;
	}
	if ((titleIdLow & 0xF0000000) != 0x10000000)
	{
		return -1;
	}
	uint32 titleId_type = (titleIdHigh & 0xFF);

	if (titleId_type == 0x30)
	{
		// applet
		uint32 ukn = ((titleIdLow >> 12) & 0xFFFF);
		if (ukn < 0x10)
		{
			return -1;
		}
		else if (ukn <= 0x19)
		{
			uint8 appletTable10[10] = { 5,6,8,3,0xA,0xB,9,4,0xC,7 };
			return appletTable10[ukn - 0x10];
		}
		else if (ukn <= 0x20)
		{
			return -1;
		}
		else if (ukn <= 0x29)
		{
			uint8 appletTable20[10] = {5,6,8,3,0xA,0xB,9,4,0xC,7};
			return appletTable20[ukn-0x20];
		}
		return -1;
	}
	else if (titleId_type == 0x10)
	{
		// system application
		if (((titleIdLow >> 12) & 0xFFFF) == 0x40)
		{
			// Wii U menu
			return 2;
		}
		return 0xF;
	}
	else if (titleId_type == 0x00 || titleId_type == 0x02)
	{
		return 0xF;
	}
	return -1;
}

void sysappExport_SYSGetVodArgs(PPCInterpreter_t* hCPU)
{
	cemuLog_logDebug(LogType::Force, "SYSGetVodArgs() - todo");
	osLib_returnFromFunction(hCPU, 1);
}

struct SysLauncherArgs18
{
	uint64be caller_id; // titleId
	uint64be launch_title; // titleId
	uint32be mode;
	uint32be slot_id;
};

static_assert(sizeof(SysLauncherArgs18) == 0x18);

struct SysLauncherArgs28
{
	uint32 ukn00;
	uint32 ukn04;
	uint32 ukn08;
	uint32 ukn0C;
	// standard args above
	uint64be caller_id; // titleId
	uint64be launch_title; // titleId
	uint32be mode;
	uint32be slot_id;
};

static_assert(sizeof(SysLauncherArgs28) == 0x28);

uint32 _SYSGetLauncherArgs(void* argsOut)
{
	uint32 sdkVersion = coreinit::__OSGetProcessSDKVersion();
	if(sdkVersion < 21103)
	{
		// old format
		SysLauncherArgs18* launcherArgs18 = (SysLauncherArgs18*)argsOut;
		memset(launcherArgs18, 0, sizeof(SysLauncherArgs18));
	}
	else
	{
		// new format
		SysLauncherArgs28* launcherArgs28 = (SysLauncherArgs28*)argsOut;
		memset(launcherArgs28, 0, sizeof(SysLauncherArgs28));
	}
	return 0; // return argument is todo
}

struct SysAccountArgs18
{
	uint32be ukn00;
	uint32be ukn04;
	uint32be ukn08;
	uint32be ukn0C;
	// shares part above with Standard arg
	uint32be slotId; // "slot_id"
	uint32be mode; // "mode"
};

uint32 _SYSGetAccountArgs(SysAccountArgs18* argsOut)
{
	memset(argsOut, 0, sizeof(SysAccountArgs18));

	// sysDeserializeStandardArguments_t ?

	return 0;
}

void sysappExport_SYSGetStandardResult(PPCInterpreter_t* hCPU)
{
	cemuLog_logDebug(LogType::Force, "SYSGetStandardResult(0x{:08x},0x{:08x},0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	memset(memory_getPointerFromVirtualOffset(hCPU->gpr[3]), 0, 4);

	// r3 = uint32be* output
	// r4 = pointer to data to parse?
	// r5 = size to parse?

	osLib_returnFromFunction(hCPU, 0);
}

namespace sysapp
{
	void SYSClearSysArgs()
	{
		cemuLog_logDebug(LogType::Force, "SYSClearSysArgs()");
		coreinit::__OSClearCopyData();
	}

	uint32 _SYSLaunchTitleByPathFromLauncher(const char* path, uint32 pathLength)
	{
		coreinit::__OSClearCopyData();
		_SYSAppendCallerInfo();
		return coreinit::OSLaunchTitleByPathl(path, pathLength, 0);
	}

	uint32 SYSRelaunchTitle(uint32 argc, MEMPTR<char>* argv)
	{
		// calls ACPCheckSelfTitleNotReferAccountLaunch?
		coreinit::__OSClearCopyData();
		_SYSAppendCallerInfo();
		return coreinit::OSRestartGame(argc, argv);
	}

	struct EManualArgs
	{
		sysStandardArguments_t stdArgs;
		uint64be titleId;
	};
	static_assert(sizeof(EManualArgs) == 0x10);

	void _SYSSwitchToEManual(EManualArgs* args)
	{
		// the struct has the titleId at offset 8 and standard args at 0 (total size is most likely 0x10)
		cemuLog_log(LogType::Force, "SYSSwitchToEManual called. Opening the manual is not supported");
		coreinit::StartBackgroundForegroundTransition();
	}

	void SYSSwitchToEManual()
	{
		EManualArgs args{};
		args.titleId = coreinit::OSGetTitleID();
		_SYSSwitchToEManual(&args);
	}

	void load()
	{
		cafeExportRegisterFunc(SYSClearSysArgs, "sysapp", "SYSClearSysArgs", LogType::Placeholder);
		cafeExportRegisterFunc(_SYSLaunchTitleByPathFromLauncher, "sysapp", "_SYSLaunchTitleByPathFromLauncher", LogType::Placeholder);
		cafeExportRegisterFunc(SYSRelaunchTitle, "sysapp", "SYSRelaunchTitle", LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToEManual, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSSwitchToEManual, LogType::Placeholder);
	}
}

namespace sysapp
{
	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "sysapp";
		}

		void RPLMapped() override
		{
			osLib_addFunction("sysapp", "_SYSLaunchMiiStudio", sysappExport__SYSLaunchMiiStudio);
			osLib_addFunction("sysapp", "_SYSGetMiiStudioArgs", sysappExport__SYSGetMiiStudioArgs);
			osLib_addFunction("sysapp", "_SYSGetSettingsArgs", sysappExport__SYSGetSettingsArgs);
			osLib_addFunction("sysapp", "_SYSReturnToCallerWithStandardResult", sysappExport__SYSReturnToCallerWithStandardResult);

			cafeExportRegisterFunc(_SYSGetSystemApplicationTitleId, "sysapp", "_SYSGetSystemApplicationTitleId", LogType::Placeholder);
			cafeExportRegisterFunc(_SYSGetSystemApplicationTitleId, "sysapp", "SYSGetSystemApplicationTitleId", LogType::Placeholder);
			cafeExportRegisterFunc(SYSGetUPIDFromTitleID, "sysapp", "SYSGetUPIDFromTitleID", LogType::Placeholder);
			cafeExportRegisterFunc(SYSGetUPIDFromTitleID, "sysapp", "_SYSGetUPIDFromTitleID", LogType::Placeholder);

			osLib_addFunction("sysapp", "_SYSGetEShopArgs", sysappExport__SYSGetEShopArgs);

			osLib_addFunction("sysapp", "SYSGetVodArgs", sysappExport_SYSGetVodArgs);

			osLib_addFunction("sysapp", "SYSGetStandardResult", sysappExport_SYSGetStandardResult);

			cafeExportRegisterFunc(_SYSGetLauncherArgs, "sysapp", "_SYSGetLauncherArgs", LogType::Placeholder);
			cafeExportRegisterFunc(_SYSGetAccountArgs, "sysapp", "_SYSGetAccountArgs", LogType::Placeholder);

			sysapp::load();
		};

	}s_COSsysappModule;

	COSModule* GetModule()
	{
		return &s_COSsysappModule;
	}
}