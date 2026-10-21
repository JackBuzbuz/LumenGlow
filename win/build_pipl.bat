@echo off
rem Compiles LumenGlowPiPL.r into a Windows .rc, the same way the SDK samples do:
rem preprocess with cl, convert with the SDK's PiPLtool, preprocess again.
rem Called by CMake: build_pipl.bat <cl.exe> <AE SDK dir> <src dir> <build dir>

setlocal
set "CL_EXE=%~1"
set "SDK=%~2"
set "SRC=%~3"
set "OUT=%~4"

"%CL_EXE%" /nologo /I "%SDK%\Examples\Headers" /I "%SDK%\Examples\Headers\SP" /I "%SDK%\Examples\Resources" /I "%SRC%" /EP "%SRC%\LumenGlowPiPL.r" > "%OUT%\LumenGlowPiPL.rr" || exit /b 1
"%SDK%\Examples\Resources\PiPLtool.exe" "%OUT%\LumenGlowPiPL.rr" "%OUT%\LumenGlowPiPL.rrc" || exit /b 1
"%CL_EXE%" /nologo /D "MSWindows" /EP "%OUT%\LumenGlowPiPL.rrc" > "%OUT%\LumenGlowPiPL.rc" || exit /b 1
exit /b 0
