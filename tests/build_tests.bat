@echo off
rem Builds and runs the test harness. Pass a Discord application ID to also
rem publish a live presence for ~30 seconds:
rem
rem   build_tests.bat                    offline checks + handshake probe
rem   build_tests.bat 123456789012345678 also sets a real presence
setlocal

set "ROOT=%~dp0.."
set "OUTDIR=%ROOT%\build\tests"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo ERROR: Visual Studio with the C++ toolset was not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /utf-8 /DWIN32 /DUNICODE /D_UNICODE ^
   /D_CRT_SECURE_NO_WARNINGS /Fo"%OUTDIR%\\" /Fe"%OUTDIR%\test_ipc.exe" ^
   "%ROOT%\tests\test_ipc.cpp" ^
   "%ROOT%\src\discord_ipc.cpp" ^
   "%ROOT%\src\artwork.cpp" ^
   "%ROOT%\src\coverart.cpp" ^
   "%ROOT%\src\upload.cpp" ^
   "%ROOT%\src\http.cpp" ^
   "%ROOT%\src\util.cpp" ^
   kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib ^
   winhttp.lib shlwapi.lib gdiplus.lib crypt32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo.
"%OUTDIR%\test_ipc.exe" %1 %2
endlocal
