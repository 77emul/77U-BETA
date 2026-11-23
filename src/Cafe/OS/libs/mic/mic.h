#include "Cafe/OS/RPL/COSModule.h"

bool mic_isActive(uint32 drcIndex);
void mic_updateOnAXFrame();
// The permission/route callback may run off the audio and guest threads.
void mic_requestInputDeviceRefresh();

namespace mic
{
	COSModule* GetModule();
};
