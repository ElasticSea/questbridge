# Configure, build and test the native library on Windows with Visual Studio's bundled CMake.
# Usage: native\build.ps1 [-Config Debug|Release]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$bin = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$src = $PSScriptRoot
& "$bin\cmake.exe" -S $src -B "$src\build"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$bin\cmake.exe" --build "$src\build" --config $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$bin\ctest.exe" --test-dir "$src\build" -C $Config --output-on-failure
exit $LASTEXITCODE
