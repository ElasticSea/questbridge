# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
# Download Cisco's prebuilt OpenH264 DLL (the software encoder fallback) on this PC.
# Cisco pays the H.264 patent royalties only for its own binary fetched from ciscobinary.openh264.org
# by the end user, so it is fetched per machine, never shipped with our builds.
# Usage: native\encoders\openh264\fetch.ps1  -> %LOCALAPPDATA%\QuestBridge\openh264\openh264-<ver>-win64.dll
$ErrorActionPreference = 'Stop'
$ver = '2.7.0'  # must match include/wels/codec_ver.h
$sha = '0ef5e7f3b38753b15c13afa0418d4d7805fa228e6adfd3e425bbf758cff1c288'  # openh264-2.7.0-win64.dll.signed.sha256.txt
$name = "openh264-$ver-win64.dll"
$dir = Join-Path $env:LOCALAPPDATA 'QuestBridge\openh264'
$dll = Join-Path $dir $name
if ((Test-Path $dll) -and (Get-FileHash $dll -Algorithm SHA256).Hash -eq $sha) { Write-Output $dll; exit 0 }
New-Item -ItemType Directory -Force $dir | Out-Null
$bz2 = "$dll.bz2"
Invoke-WebRequest "https://ciscobinary.openh264.org/$name.bz2" -OutFile $bz2 -UseBasicParsing
# Store Python virtualizes AppData writes, so it only sees stdin/stdout
cmd /c ('py -I -c "import bz2,sys; sys.stdout.buffer.write(bz2.decompress(sys.stdin.buffer.read()))" < "{0}" > "{1}"' -f $bz2, $dll)
Remove-Item $bz2
if ((Get-FileHash $dll -Algorithm SHA256).Hash -ne $sha) { Remove-Item $dll; throw "${name}: SHA-256 mismatch" }
Write-Output $dll
