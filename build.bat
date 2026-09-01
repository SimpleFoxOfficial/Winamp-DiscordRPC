@echo off
rem ---------------------------------------------------------------------------
rem Builds gen_discord_rpc.dll (32-bit -- Winamp is a 32-bit host).
rem
rem Usage:  build.bat [install]
rem   install  also copies the DLL into the Winamp Plugins folder.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "OUTDIR=%ROOT%build"

rem --- locate Visual Studio -------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2019 or newer
    echo        with the "Desktop development with C++" workload.
    exit /b 1
)

rem Kept on one line because a ^ continuation inside a backquoted FOR /f block
rem is consumed by the sub-shell rather than continuing the command.
rem
rem Note: vcvars32.bat below may print "'vswhere.exe' is not recognized" on some
rem Visual Studio installs. That comes from Microsoft's own script, is harmless,
rem and leaves errorlevel at 0 -- the build is unaffected.
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo ERROR: no Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

rem vcvars32 targets x86, which is what Winamp loads.
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo ERROR: failed to initialise the 32-bit build environment.
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo Compiling resources...
rc /nologo /fo "%OUTDIR%\plugin.res" /i "%ROOT%src" "%ROOT%src\plugin.rc"
if errorlevel 1 exit /b 1

echo Compiling...
rem /utf-8 so the sources are read as UTF-8 regardless of the system codepage.
cl /nologo /c /O2 /MT /W4 /EHsc /std:c++17 /utf-8 /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE ^
   /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Fo"%OUTDIR%\\" ^
   "%ROOT%src\plugin.cpp" ^
   "%ROOT%src\config.cpp" ^
   "%ROOT%src\util.cpp" ^
   "%ROOT%src\http.cpp" ^
   "%ROOT%src\artwork.cpp" ^
   "%ROOT%src\coverart.cpp" ^
   "%ROOT%src\upload.cpp" ^
   "%ROOT%src\discord_ipc.cpp" ^
   "%ROOT%src\winamp_state.cpp"
if errorlevel 1 exit /b 1

echo Linking...
link /nologo /DLL /MACHINE:X86 /OUT:"%OUTDIR%\gen_discord_rpc.dll" ^
     /DEF:"%ROOT%src\gen_discord_rpc.def" ^
     "%OUTDIR%\plugin.obj" "%OUTDIR%\config.obj" "%OUTDIR%\util.obj" ^
     "%OUTDIR%\http.obj" "%OUTDIR%\artwork.obj" "%OUTDIR%\coverart.obj" ^
     "%OUTDIR%\upload.obj" "%OUTDIR%\discord_ipc.obj" ^
     "%OUTDIR%\winamp_state.obj" "%OUTDIR%\plugin.res" ^
     kernel32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib ^
     oleaut32.lib winhttp.lib advapi32.lib comctl32.lib comdlg32.lib gdiplus.lib crypt32.lib
if errorlevel 1 exit /b 1

echo.
echo Built %OUTDIR%\gen_discord_rpc.dll

if /i "%~1"=="install" (
    set "WAPLUGINS=%ProgramFiles(x86)%\Winamp\Plugins"
    if not exist "!WAPLUGINS!" set "WAPLUGINS=%ProgramFiles%\Winamp\Plugins"
    if not exist "!WAPLUGINS!" (
        echo ERROR: could not find the Winamp Plugins folder.
        exit /b 1
    )
    echo Installing to !WAPLUGINS! ...
    copy /y "%OUTDIR%\gen_discord_rpc.dll" "!WAPLUGINS!\gen_discord_rpc.dll" >nul
    if errorlevel 1 (
        echo ERROR: copy failed. Close Winamp and try again ^(the DLL is locked
        echo        while Winamp is running^), or run this from an elevated prompt.
        exit /b 1
    )
    echo Installed. Restart Winamp, then open
    echo   Options ^> Preferences ^> Plug-ins ^> General Purpose
    echo to configure it.
)

endlocal
