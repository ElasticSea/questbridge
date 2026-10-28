# Build the encoder library and qbenc with the VS 2026 toolchain (CMake + Ninja bundled with VS).
# Usage: native\encoders\build.ps1   -> native\encoders\build\qbenc.exe
$ErrorActionPreference = 'Stop'
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH = "$installer;$env:PATH"  # vcvars64.bat calls vswhere
$vs = & "$installer\vswhere.exe" -latest -products * -property installationPath
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$src = $PSScriptRoot
$build = Join-Path $src 'build'
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cmake`" -S `"$src`" -B `"$build`" -G Ninja " +
       "-DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE) { exit $LASTEXITCODE }
