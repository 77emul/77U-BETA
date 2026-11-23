#pragma once
#include "Cafe/OS/RPL/COSModule.h"
#include <string>
#include <string_view>

void swkbd_render(bool mainWindow);
bool swkbd_hasKeyboardInputHook();
void swkbd_keyInput(uint32 keyCode);
void swkbd_finishInput();
void swkbd_setText(std::string_view utf8Text);
std::string swkbd_getText();
int32_t swkbd_getMaxTextLength();

namespace swkbd
{
	COSModule* GetModule();
}

