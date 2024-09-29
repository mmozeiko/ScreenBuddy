@echo off
setlocal enabledelayedexpansion

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )  
  call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" amd64 || exit /b 1
)

if "%1" equ "debug" (
  set CL=/MTd /Od /Zi /D_DEBUG /RTC1 /FdScreenBuddy.pdb /fsanitize=address
  set LINK=/DEBUG
) else (
  set CL=/GL /O1 /Oi /DNDEBUG /GS-
  set LINK=/LTCG /OPT:REF /OPT:ICF ucrt.lib libvcruntime.lib
)

fxc.exe /nologo /T vs_5_0 /E VS /O3 /WX /Ges /Fh ScreenBuddyVS.h /Vn ScreenBuddyVS /Qstrip_reflect /Qstrip_debug /Qstrip_priv ScreenBuddy.hlsl || exit /b 1
fxc.exe /nologo /T ps_5_0 /E PS /O3 /WX /Ges /Fh ScreenBuddyPS.h /Vn ScreenBuddyPS /Qstrip_reflect /Qstrip_debug /Qstrip_priv ScreenBuddy.hlsl || exit /b 1

rc.exe /nologo ScreenBuddy.rc || exit /b 1
cl.exe /nologo /W3 /WX ScreenBuddy.c ScreenBuddy.res /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:ScreenBuddy.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata || exit /b 1
del *.obj *.res >nul
