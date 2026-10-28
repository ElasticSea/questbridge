# Proof run for tools\unity_hidden.ps1: logs the foreground window every 200 ms (focus_watch.ps1, in a
# background job) while test_all.ps1 runs every suite including Unity, then reports whether a Unity
# process ever took the foreground. Exit 0 only if the tests passed and nothing was STOLEN.
#   .\verify_unity_hidden.ps1 [-Only unity]
param([string]$Only = "unity")
$repo = Split-Path $PSScriptRoot -Parent
$focusLog = Join-Path $repo ("capture\runs\focus-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
New-Item -ItemType Directory -Force (Split-Path $focusLog) | Out-Null
$job = Start-Job -ScriptBlock {
    param($script, $out)
    & $script -Seconds 1800 -Out $out
} -ArgumentList (Join-Path $PSScriptRoot "focus_watch.ps1"), $focusLog
Start-Sleep 2
$testArgs = @{}
if ($Only) { $testArgs.Only = $Only }
& (Join-Path $PSScriptRoot "test_all.ps1") @testArgs
$testCode = $LASTEXITCODE
Stop-Job $job; Remove-Job $job -Force
$stolen = @(Select-String -Path $focusLog -Pattern "STOLEN").Count
$fg = @(Get-Content $focusLog | Select-Object -Skip 1 | ForEach-Object { ($_ -split ' ', 3)[2] } | Sort-Object -Unique)
Write-Host "foreground owners during the run: $($fg -join ', ')"
Write-Host "focus log: $focusLog  STOLEN samples: $stolen  tests exit: $testCode"
exit [int]($stolen -gt 0 -or $testCode -ne 0)
