# Run every test suite in the repo and print a pass/fail summary.
# Usage: tools\test_all.ps1 [-Quick] [-Only py|native|unity] [-UnityTimeout <s>]
#   -Quick  Python + native only (before each commit). Full run (with Unity) before merging to main.
# Suites: py (unittest), native (native\build.ps1: Release build incl. encoders, then CTest; the full
#         run adds tools\slice_cap_stress.py: high-entropy clips through qlenc, slices under the cap),
#         unity (BridgeTest EditMode + PlayMode in batchmode, PlayMode again in a minimized
#         non-batchmode Editor; skipped if Unity is missing). Unity runs
#         with QL_USB=0 (questlink.dll never opens the headset) and a D3D11 device (Editor Play tests).
# Logs: out\runs\test_all\<stamp>\<step>.log. Exit code 0 only if no step failed.
param(
    [switch]$Quick,
    [ValidateSet("", "py", "native", "unity")][string]$Only = "",
    [int]$UnityTimeout = 1800
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$unityExe = "C:\Program Files\Unity\Hub\Editor\6000.3.11f1\Editor\Unity.exe"
$logDir = Join-Path $repo ("out\runs\test_all\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $logDir | Out-Null
$results = @()

# Run a process with stdout+stderr into $log; returns the exit code (-1 on timeout).
function Invoke-Logged($exe, [string]$argLine, $log, [int]$timeoutS = 1800) {
    $p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $repo -NoNewWindow -PassThru `
        -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
    $null = $p.Handle  # keeps ExitCode readable in PowerShell 5.1
    if (-not $p.WaitForExit($timeoutS * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $code = -1
    } else { $code = $p.ExitCode }
    Get-Content "$log.out", "$log.err" -ErrorAction SilentlyContinue | Set-Content $log
    Remove-Item "$log.out", "$log.err" -ErrorAction SilentlyContinue
    return $code
}

# Unity only ever runs through unity_hidden.ps1 (on a hidden desktop, so it can't take the user's
# focus). Unity writes its own -logFile; returns the exit code (-1 on timeout).
function Invoke-Unity([string]$argLine, $log, [int]$timeoutS = 1800) {
    & "$PSScriptRoot\unity_hidden.ps1" -Exe $unityExe -ArgLine $argLine -WorkingDirectory $repo -TimeoutS $timeoutS *> $log
    return $LASTEXITCODE
}

# Append Unity's -logFile to the step log. Unity's helper processes can hold the file for a
# moment after the editor exits, so retry the delete instead of failing the run.
function Merge-UnityLog($log) {
    if (-not (Test-Path "$log.unity")) { return }
    Get-Content "$log.unity" | Add-Content $log
    for ($i = 0; $i -lt 20; $i++) {
        try { Remove-Item "$log.unity" -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 500 }
    }
}

function Add-Result($name, $status, $seconds, $detail, $log) {
    $script:results += [pscustomobject]@{ Step = $name; Status = $status; Seconds = [math]::Round($seconds, 1); Detail = $detail; Log = $log }
    $color = @{ PASS = "Green"; FAIL = "Red"; SKIP = "Yellow" }[$status]
    Write-Host ("[{0}] {1,-26} {2,6:N1}s  {3}" -f $status, $name, $seconds, $detail) -ForegroundColor $color
    if ($status -eq "FAIL" -and $log -and (Test-Path $log)) {
        Get-Content $log -Tail 25 | ForEach-Object { Write-Host "    $_" }
        Write-Host "    full log: $log"
    }
}

function Test-Python {
    $log = "$logDir\py.log"; $t = Get-Date
    $code = Invoke-Logged "py" "-m unittest discover -s tests" $log
    $text = Get-Content $log -Raw
    $detail = if ($text -match "Ran (\d+) tests?") { "$($Matches[1]) tests" } else { "no test count" }
    if ($text -match "FAILED \(([^)]*)\)") { $detail += ", $($Matches[1])" }
    if ($text -match "skipped=(\d+)") { $detail += " ($($Matches[1]) skipped)" }
    Add-Result "py" $(if ($code -eq 0) { "PASS" } else { "FAIL" }) ((Get-Date) - $t).TotalSeconds $detail $log
}

function Test-Native {
    # native\build.ps1 builds everything under native\ (encoders are a subdirectory) and runs all CTest tests.
    $log = "$logDir\native.log"; $t = Get-Date
    $code = Invoke-Logged "powershell" "-NoProfile -ExecutionPolicy Bypass -File `"$repo\native\build.ps1`" -Config Release" $log
    $text = Get-Content $log -Raw
    $detail = if ($text -match "(\d+)% tests passed, (\d+) tests? failed out of (\d+)") {
        "$([int]$Matches[3] - [int]$Matches[2])/$($Matches[3]) ctest passed"
    } else { "build failed (exit $code)" }
    Add-Result "native" $(if ($code -eq 0) { "PASS" } else { "FAIL" }) ((Get-Date) - $t).TotalSeconds $detail $log
}

# Full run: high-entropy clips through qlenc on the GPU encoder; every slice must fit one XRSP message.
function Test-SliceCap {
    $log = "$logDir\slicecap.log"; $t = Get-Date
    $code = Invoke-Logged "py" "tools\slice_cap_stress.py" $log
    $text = Get-Content $log -Raw
    $detail = if ($text -match "all (\d+) cases under the cap") { "$($Matches[1]) qlenc cases under the cap" }
              elseif ($text -match "FAILED: (\d+) of (\d+)") { "$($Matches[1]) of $($Matches[2]) cases over the cap" }
              else { "qlenc did not run (exit $code)" }
    Add-Result "slicecap" $(if ($code -eq 0) { "PASS" } else { "FAIL" }) ((Get-Date) - $t).TotalSeconds $detail $log
}

# One Unity test platform, NUnit XML parsed into counts. Exit codes: 0 pass, 2 failures, 3 run error.
# -Window: a real Editor (still on the hidden desktop), so cameras render as in Play mode.
function Test-UnityTests($project, $platform, $assemblies, [switch]$Window) {
    $name = "unity-$($project.ToLower())-$($platform.ToLower())" + $(if ($Window) { "-window" } else { "" })
    $log = "$logDir\$name.log"; $xml = "$logDir\$name.xml"; $t = Get-Date
    $proj = "$repo\unity\$project"
    if (Test-Path "$proj\Temp\UnityLockfile") {
        try { [IO.File]::Open("$proj\Temp\UnityLockfile", "Open", "ReadWrite", "None").Close() }
        catch { Add-Result $name "FAIL" 0 "project is open in another Unity Editor; close it" $null; return }
    }
    $code = Invoke-Unity ($(if ($Window) { "" } else { "-batchmode " }) + "-projectPath `"$proj`" -runTests -testPlatform $platform " +
        "-assemblyNames $assemblies -testResults `"$xml`" -logFile `"$log.unity`"") $log $UnityTimeout
    Merge-UnityLog $log
    $status = "FAIL"
    if (Test-Path $xml) {
        $run = ([xml](Get-Content $xml -Raw)).'test-run'
        $detail = "$($run.passed)/$($run.total) passed"
        if ([int]$run.failed) { $detail += ", $($run.failed) failed" }
        if ([int]$run.skipped) { $detail += ", $($run.skipped) skipped" }
        if ($code -eq 0 -and [int]$run.total -gt 0 -and [int]$run.failed -eq 0) { $status = "PASS" }
    } else { $detail = if ($code -eq -1) { "timed out after ${UnityTimeout}s" } else { "no results (exit $code; compile error?)" } }
    Add-Result $name $status ((Get-Date) - $t).TotalSeconds $detail $log
}

# Open the project in batchmode and quit: Unity exits non-zero when scripts fail to compile.
function Test-UnityCompile($project) {
    $name = "unity-$($project.ToLower())-compile"
    $log = "$logDir\$name.log"; $t = Get-Date
    $code = Invoke-Unity "-batchmode -nographics -quit -projectPath `"$repo\unity\$project`" -logFile `"$log.unity`"" $log $UnityTimeout
    Merge-UnityLog $log
    $errors = @(Select-String -Path $log -Pattern "error CS\d+" -ErrorAction SilentlyContinue).Count
    $detail = if ($code -eq 0) { "compiled" } elseif ($code -eq -1) { "timed out after ${UnityTimeout}s" } else { "exit $code, $errors compiler errors" }
    Add-Result $name $(if ($code -eq 0) { "PASS" } else { "FAIL" }) ((Get-Date) - $t).TotalSeconds $detail $log
}

function Test-Unity {
    if (-not (Test-Path $unityExe)) { Add-Result "unity" "SKIP" 0 "Unity 6000.3.11f1 not installed at $unityExe" $null; return }
    $env:QL_USB = "0"
    Test-UnityTests "BridgeTest" "EditMode" "ElasticSea.QuestLink.Tests;ElasticSea.QuestLink.XRManagement.Tests"
    Test-UnityTests "BridgeTest" "PlayMode" "ElasticSea.QuestLink.PlayModeTests;ElasticSea.QuestLink.XRManagement.PlayModeTests"
    Test-UnityTests "BridgeTest" "PlayMode" "ElasticSea.QuestLink.PlayModeTests" -Window
}

$total = Get-Date
$suites = if ($Only) { @($Only) } elseif ($Quick) { @("py", "native") } else { @("py", "native", "unity") }
foreach ($s in $suites) {
    switch ($s) {
        "py" { Test-Python }
        "native" { Test-Native; if (-not $Quick) { Test-SliceCap } }
        "unity" { Test-Unity }
    }
}

$failed = @($results | Where-Object Status -eq "FAIL").Count
Write-Host ""
Write-Host ("{0} in {1:N1}s: {2} passed, {3} failed, {4} skipped. Logs: {5}" -f `
    $(if ($failed) { "FAILED" } else { "ALL PASSED" }), ((Get-Date) - $total).TotalSeconds,
    @($results | Where-Object Status -eq "PASS").Count, $failed, @($results | Where-Object Status -eq "SKIP").Count, $logDir) `
    -ForegroundColor $(if ($failed) { "Red" } else { "Green" })
$results | Export-Csv "$logDir\summary.csv" -NoTypeInformation
exit $(if ($failed) { 1 } else { 0 })
