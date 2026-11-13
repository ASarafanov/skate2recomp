@echo off
rem Builds setup\out\Launch Skate 2.exe with the Visual Studio 2022 toolchain.
rem Needs no game files and no ReXGlue SDK.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio 2022 or Build Tools with the C++ workload is required.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "SRC=%~dp0."
set "OUT=%~dp0out"
cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl.exe || exit /b 1
cmake --build "%OUT%" --config Release || exit /b 1
echo Built "%OUT%\Launch Skate 2.exe"
