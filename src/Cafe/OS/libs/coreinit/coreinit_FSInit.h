#pragma once

#include "Cafe/OS/libs/coreinit/coreinit_FS.h"

namespace coreinit
{
	extern bool sFSInitialized;
	extern bool sFSShutdown;

	void RegisterFSInitExport();
}
