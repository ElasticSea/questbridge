# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
# Build questbridge.dll (Release) and install it as the Unity package's Windows x64 native plugin:
# com.elasticsea.questbridge/Runtime/Plugins/Windows/x86_64/questbridge.dll (git-ignored; releases ship it as a download, so UPM git installs
# get it: commit the new binary after a native change that the package needs).
# Close every Unity Editor that loaded the plugin first: Windows keeps a loaded DLL locked.
# Usage: unity\install-native.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not $NoBuild) {
    & "$repo\native\build.ps1" -Config Release
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$dll = "$repo\native\build\bin\Release\questbridge.dll"
if (-not (Test-Path $dll)) { throw "$dll not found; run native\build.ps1" }

$dest = "$PSScriptRoot\com.elasticsea.questbridge\Runtime\Plugins\Windows\x86_64"
New-Item -ItemType Directory -Force $dest | Out-Null
try {
    Copy-Item $dll "$dest\questbridge.dll" -Force
} catch {
    throw "Could not replace $dest\questbridge.dll; close the Unity Editor that has it loaded. ($_)"
}

# Import settings: Editor and Standalone, Windows x64 only. Fixed GUID so reinstalls keep references.
$meta = @"
fileFormatVersion: 2
guid: 6b1f0c2e9a4d4e7f8c3b5a2d1e0f9a87
PluginImporter:
  externalObjects: {}
  serializedVersion: 3
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 1
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
    Any:
      enabled: 0
      settings: {}
    Editor:
      enabled: 1
      settings:
        CPU: x86_64
        DefaultValueInitialized: true
        OS: Windows
    Win:
      enabled: 0
      settings:
        CPU: None
    Win64:
      enabled: 1
      settings:
        CPU: x86_64
    WindowsStoreApps:
      enabled: 0
      settings:
        CPU: x64
  userData: 
  assetBundleName: 
  assetBundleVariant: 
"@
[IO.File]::WriteAllText("$dest\questbridge.dll.meta", $meta.Replace("`r`n", "`n"))
Write-Host "Installed $dest\questbridge.dll"
