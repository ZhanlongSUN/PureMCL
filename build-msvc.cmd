@echo off
setlocal EnableExtensions
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto build
set "NMC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%NMC_VSWHERE%" goto missing_msvc
for /f "usebackq delims=" %%V in (`"%NMC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NMC_VS=%%V"
if not defined NMC_VS goto missing_msvc
call "%NMC_VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
:build
where cmake.exe >nul 2>nul || goto missing_cmake
if defined NMC_NINJA_EXE goto check_ninja
for /f "delims=" %%N in ('where ninja.exe 2^>nul') do if not defined NMC_NINJA_EXE set "NMC_NINJA_EXE=%%N"
if defined NMC_NINJA_EXE goto check_ninja
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NMC_NINJA_EXE=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if defined NMC_NINJA_EXE goto check_ninja
rem CLion's bundled Ninja is not normally on PATH. Read the registered install directory.
for /f "usebackq delims=" %%N in (`powershell.exe -NoProfile -Command "foreach ($key in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*','HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*')) { foreach ($app in (Get-ItemProperty $key -ErrorAction SilentlyContinue)) { if ($app.DisplayName -like 'CLion*' -and $app.InstallLocation) { $n = Join-Path $app.InstallLocation 'bin\ninja\win\x64\ninja.exe'; if ([IO.File]::Exists($n)) { $n; exit } } } }"`) do set "NMC_NINJA_EXE=%%N"
:check_ninja
if not defined NMC_NINJA_EXE goto missing_ninja
if not exist "%NMC_NINJA_EXE%" goto missing_ninja
set "NMC_BUILD_TYPE=%~1"
if not defined NMC_BUILD_TYPE set "NMC_BUILD_TYPE=Release"
set "NMC_BUILD_DIR=%~dp0out\%NMC_BUILD_TYPE%"
echo Using Ninja: "%NMC_NINJA_EXE%"
cmake -S "%~dp0." -B "%NMC_BUILD_DIR%" -G Ninja "-DCMAKE_BUILD_TYPE=%NMC_BUILD_TYPE%" "-DCMAKE_MAKE_PROGRAM:FILEPATH=%NMC_NINJA_EXE%" || exit /b 1
cmake --build "%NMC_BUILD_DIR%" || exit /b 1
ctest --test-dir "%NMC_BUILD_DIR%" --output-on-failure || exit /b 1
if /i not "%NMC_BUILD_TYPE%"=="Release" goto built
cmake --install "%NMC_BUILD_DIR%" --prefix "%~dp0out\portable" || exit /b 1
echo Portable launcher: "%~dp0out\portable\PureMCL.exe"
:built
exit /b 0
:missing_msvc
echo MSVC x64 toolchain not found.
exit /b 1
:missing_cmake
echo CMake not found. Add its bin directory to PATH.
exit /b 1
:missing_ninja
echo Ninja not found. Add it to PATH or set NMC_NINJA_EXE to the full path of ninja.exe.
exit /b 1

