@echo off
setlocal
cd /d "%~dp0"
where msbuild >nul 2>nul
if not errorlevel 1 (
  msbuild Windows\VisualStudio\vkquake.sln /m /p:Configuration=Release /p:Platform=x64 %*
  exit /b
)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
  "%%i" Windows\VisualStudio\vkquake.sln /m /p:Configuration=Release /p:Platform=x64 %*
  exit /b
)
echo MSBuild not found. Install the Visual Studio C++ build tools.
exit /b 1
