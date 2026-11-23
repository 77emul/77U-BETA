#pragma once

#include "Cafe/HW/MMU/MMU.h"

MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment);
void coreinit_freeToSysArea(MPTR mem);
