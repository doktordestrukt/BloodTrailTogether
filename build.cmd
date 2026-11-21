@echo off
rem Developer build: native library, game DLL, SimPeer, tests (Release, x64).
rem Usage: build.cmd [release]   ("release" = public build with developer features hidden)
setlocal
set ROOT=%~dp0
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio 2022 with C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CMAKE="%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NINJA_DIR=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
set PATH=%NINJA_DIR%;%PATH%

rem Build trees live in a short path: MSVC still trips over the 260-char path limit.
if not defined BTT_BUILD_ROOT set BTT_BUILD_ROOT=%LOCALAPPDATA%\BTTBuild
set BUILD_DIR=%BTT_BUILD_ROOT%\dev
set REL=OFF
if /i "%1"=="release" (
  set BUILD_DIR=%BTT_BUILD_ROOT%\release
  set REL=ON
)
%CMAKE% -S "%ROOT%." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBTT_RELEASE_BUILD=%REL% "-DBTT_PAYLOAD=%BTT_PAYLOAD%" || exit /b 1
%CMAKE% --build "%BUILD_DIR%" %2 %3 || exit /b 1
echo Build OK: %BUILD_DIR%
