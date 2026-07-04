#pragma once

#include <Windows.h>
#include "ControllerSpecs/Controller #1.1.h"

enum LunaExCommand
{
	LUNA_EXCMD_LOAD_STATE,
	LUNA_EXCMD_SAVE_STATE,
	LUNA_EXCMD_UNLOCK_FPS,
	LUNA_EXCMD_LOCK_FPS,
};

typedef void(CALL* LunaExCommandFn)(HWND, LunaExCommand);

namespace Luna
{
	extern HWND gMainWindow;
	extern LunaExCommandFn gExCommandHandler;
}
