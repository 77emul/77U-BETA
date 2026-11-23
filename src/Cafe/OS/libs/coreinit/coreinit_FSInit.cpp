#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_FSInit.h"
#include "Cafe/OS/RPL/rpl.h"

namespace coreinit
{
	bool sFSInitialized = true; // this should be false but it seems like some games rely on FSInit being called before main()? Twilight Princess for example reads files before it calls FSInit
	bool sFSShutdown = false;

	void FSInit()
	{
		sFSInitialized = true;
	}

	void FSShutdown()
	{
		cemuLog_log(LogType::Force, "FSShutdown called");
	}

	void RegisterFSInitExport()
	{
		cafeExportRegister("coreinit", FSInit, LogType::CoreinitFile);
		cafeExportRegister("coreinit", FSShutdown, LogType::CoreinitFile);
	}
}
