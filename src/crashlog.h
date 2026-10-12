#pragma once
#include "util.h"

// Spec 4.10: on an internal bug, stop and say why in %TEMP%\qoqdir-crash.log. Call first in wWinMain.
void installCrashLog();
void crashLogIn(const std::wstring& dir);   // the log goes into this folder from now on (a self-test keeps its own)
// Once Windows calls the window not responding (5 s without a message read), logs where its thread is: each frame of
// its stack as an exe offset or a DLL name, so a hung shell extension names itself. Call from the window's thread.
void watchForHangs(HWND window);
