# Builds FirstLight (hidden Unity) and checks a real rendered frame: the player runs with the mock
# backend (no USB), writes its eye texture to a PNG and fails if it is magenta-dominant (missing
# shaders) or one flat colour. Exit 0 = build OK and picture OK. Look at the PNG too.
# -Mode checks one bench mode (-sequence-look, -sequence-recovery, ...); default: plain scene.
#   .\check_firstlight.ps1 [-SkipBuild] [-Mode -sequence-look] [-Out out\runs\firstlight-snapshot.png]
param([switch]$SkipBuild, [string]$Mode = "", [string]$Out = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Join-Path $repo "out\runs\firstlight-snapshot.png" }
$runs = Split-Path $Out
New-Item -ItemType Directory -Force $runs | Out-Null
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "unity_hidden.ps1") -WorkingDirectory $repo -ArgLine "-batchmode -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.BuildFirstLight.Build -logFile `"$runs\firstlight-build.log`""
    if ($LASTEXITCODE -ne 0) { Write-Host "build failed ($LASTEXITCODE), see $runs\firstlight-build.log"; exit 1 }
}
$log = Join-Path $runs "firstlight-snapshot.log"
$ppm = [IO.Path]::ChangeExtension($Out, ".ppm")
Remove-Item $Out, $ppm, $log -ErrorAction SilentlyContinue
$env:QL_USB = "0"
& (Join-Path $PSScriptRoot "start_player.ps1") -Exe (Join-Path $repo "unity\BridgeTest\Build\FirstLight\FirstLight.exe") `
    -ArgLine "$Mode -mock -snapshot `"$ppm`" -seconds 60 -screen-fullscreen 0 -logFile `"$log`"" -TimeoutS 90 | Out-Null
$code = $LASTEXITCODE
if (Test-Path $ppm) { ffmpeg -loglevel error -y -i $ppm -vf scale=1024:-1 $Out; Remove-Item $ppm }
$line = Select-String $log -Pattern "\[Snapshot\]" | Select-Object -Last 1
Write-Host "$(if ($line) { $line.Line } else { 'no [Snapshot] line' }) (exit $code)"
exit [int](-not $line -or $code -ne 0)
