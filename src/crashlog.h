#pragma once
// Spec 4.10: when the program hits an internal bug, it stops and %TEMP%\qoqdir-crash.log says why.
// Call once, first thing in wWinMain.
void installCrashLog();
