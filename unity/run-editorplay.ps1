# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
# Live Editor Play-mode bench (BridgeTest Assets/Editor/EditorPlayBench.cs): a real, minimized
# Unity Editor on tools\unity_hidden.ps1's hidden desktop drops the QuestBridge Rig prefab into a
# fresh scene and plays two rounds with a Stop in between, steps and questions on the headset
# board. Blocks until the Editor exits (run the script in the background). Logs go to
# out\runs\editorplay-<stamp>\ (editor.log: "[EditorPlay]" lines; native.log: protocol).
#   .\run-editorplay.ps1          headset (stop OVRService first: tools\windows\ovr_service.ps1 stop)
#   .\run-editorplay.ps1 -Mock    dry run on the mock headset, never touches USB
param([switch]$Mock, [int]$TimeoutS = 3700)

$root = Split-Path $PSScriptRoot
$dir = Join-Path $root ("out\runs\editorplay-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $dir | Out-Null
$env:QB_LOG = "1"
$env:QB_LOG_FILE = Join-Path $dir "native.log"
if ($Mock) { $env:QB_BENCH_MOCK = "1"; $env:QB_USB = "0" } else { Remove-Item Env:QB_BENCH_MOCK, Env:QB_USB -ErrorAction SilentlyContinue }
$project = Join-Path $PSScriptRoot "BridgeTest"
$argList = "-projectPath `"$project`" -runTests -testPlatform EditMode -testFilter ElasticSea.QuestBridge.BridgeTest.Editor.EditorPlayBench " +
    "-testResults `"$(Join-Path $dir 'results.xml')`" -logFile `"$(Join-Path $dir 'editor.log')`""
Write-Host "Editor Play bench logs in $dir"
& (Join-Path $root "tools\unity_hidden.ps1") -ArgLine $argList -TimeoutS $TimeoutS
exit $LASTEXITCODE
