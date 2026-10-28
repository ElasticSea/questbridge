# One headset slot, back to back, every step on the board (A to continue, questions A yes / B no):
#   1 player check   FirstLight via start_player.ps1 (minimized, no focus), "picture OK?"
#   2 recovery       FirstLight -sequence-recovery + tools\recovery_check.py (sleep, Link quit)
#   3 encoder strips FirstLight with QB_ENCODER=mf QB_LAYOUT=strips, "picture OK?"
#   4 encoder sw     FirstLight with QB_ENCODER=openh264, "picture OK?"
# Between parts the board says "press Launch when you see it". A focus watcher runs throughout.
# Needs tools\capture\bench_elevated.ps1 running (one UAC: capture + OVRService triggers) before the
# user puts the headset on. Players start minimized and
# fall back to -NoMinimize if they stream without encoding. Results: out\runs\slot-<stamp>\summary.txt.
#   .\bench_slot.ps1 [-Parts 1,2,3,4]
param([int[]]$Parts = @(1, 2, 3, 4))
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $repo ("out\runs\slot-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $dir | Out-Null
$summary = Join-Path $dir "summary.txt"
$adb = if ($env:ADB) { $env:ADB } else { "adb" }
$firstLight = Join-Path $repo "unity\BridgeTest\Build\FirstLight\FirstLight.exe"
$launch = "Link closes now.|Press Launch when you see it."
function Say($text) { $line = "$(Get-Date -Format HH:mm:ss) $text"; Write-Host $line; $line | Add-Content $summary }
function Grep($file, $pattern) { if (Test-Path $file) { @(Select-String -Path $file -Pattern $pattern | ForEach-Object { $_.Line }) } else { @() } }

$focus = Start-Job -ScriptBlock { param($s, $o) & $s -Seconds 3600 -Out $o } `
    -ArgumentList (Join-Path $PSScriptRoot "focus_watch.ps1"), (Join-Path $dir "focus.log")

# Players start minimized; if a minimized player streams but never encodes a frame (nothing in
# the headset), it is restarted with -NoMinimize and the rest of the slot keeps that
$script:noMinimize = $false
function Player($exe, [string]$argLine) {
    $job = Start-Job -ScriptBlock { param($s, $e, $a, $n) & $s -Exe $e -ArgLine $a -TimeoutS 1800 -NoMinimize:$n *>&1; "exit $LASTEXITCODE" } `
        -ArgumentList (Join-Path $PSScriptRoot "start_player.ps1"), $exe, $argLine, $script:noMinimize
    return $job
}

# One FirstLight part through start_player.ps1; returns the part's log directory
function FirstLight($name, [string]$flags, [hashtable]$envs) {
    $d = Join-Path $dir $name
    New-Item -ItemType Directory -Force $d | Out-Null
    foreach ($k in "QB_ENCODER", "QB_LAYOUT") { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $envs.Keys) { Set-Item "env:$k" $envs[$k] }
    $env:QB_LOG = "1"
    $env:QB_LOG_FILE = Join-Path $d "qb.log"
    $argLine = "$flags -screen-fullscreen 0 -screen-width 320 -screen-height 180 -logFile `"$(Join-Path $d 'player.log')`""
    $qb = Join-Path $d "qb.log"
    for ($try = 0; ; $try++) {
        Say "part $name start ($($envs.Keys | ForEach-Object { "$_=$($envs[$_])" }))$(if ($script:noMinimize) { ' -NoMinimize' })"
        $job = Player $firstLight $argLine
        $streamingAt = $null
        $restart = $false
        while ($job.State -eq "Running") {
            Start-Sleep -Milliseconds 500
            if (-not $script:noMinimize -and $try -eq 0) {
                if (-not $streamingAt -and (Grep $qb "-> streaming")) { $streamingAt = Get-Date }
                if ($streamingAt -and -not (Grep $qb "encoder:") -and ((Get-Date) - $streamingAt).TotalSeconds -gt 8) {
                    Say "part $name streaming but no frame encoded while minimized: restarting with -NoMinimize"
                    Get-Process FirstLight -ErrorAction SilentlyContinue | Stop-Process -Force
                    $restart = $true
                    break
                }
            }
        }
        Wait-Job $job -Timeout 10 | Out-Null
        Receive-Job $job *>> (Join-Path $d "launcher.log")
        Remove-Job $job -Force
        if (-not $restart) { break }
        $script:noMinimize = $true
        Rename-Item $qb "qb-minimized.log" -ErrorAction SilentlyContinue
    }
    Say "part $name done; $((Grep (Join-Path $d 'player.log') '\[Look\] answer|\[Recovery\] done') -join '; ')"
    Say "part $name encoder: $((Grep $qb 'encoder' | Select-Object -First 2) -join ' / ')"
    return $d
}

if (1 -in $Parts) {
    FirstLight "1-player" "-sequence-look -look-label `"PART 1 of 4|Player check`" -look-next `"PART 2: recovery test.|$launch`"" @{} | Out-Null
}
if (2 -in $Parts) {
    $d = Join-Path $dir "2-recovery"
    New-Item -ItemType Directory -Force $d | Out-Null
    $rec = Start-Job -ScriptBlock { param($r, $p, $q) Set-Location $r; py tools\recovery_check.py --log $p --qblog $q --timeout 1800 2>&1 } `
        -ArgumentList $repo, (Join-Path $d "player.log"), (Join-Path $d "qb.log")
    FirstLight "2-recovery" "-sequence-recovery -look-next `"PART 3: encoder test (strips).|$launch`"" @{} | Out-Null
    Wait-Job $rec -Timeout 30 | Out-Null
    Receive-Job $rec *> (Join-Path $d "recovery_check.log")
    Remove-Job $rec -Force
    Say "part 2-recovery recovery_check: $((Get-Content (Join-Path $d 'recovery_check.log') -Tail 3) -join ' | ')"
}
if (3 -in $Parts) {
    FirstLight "3-mf-strips" "-sequence-look -look-label `"PART 3 of 4|Encoder: Media Foundation strips`" -look-next `"PART 4: encoder test (software).|$launch`"" @{ QB_ENCODER = "mf"; QB_LAYOUT = "strips" } | Out-Null
}
if (4 -in $Parts) {
    FirstLight "4-openh264" "-sequence-look -look-label `"PART 4 of 4|Encoder: OpenH264 (software)`" -look-next `"All parts done.|Take the headset off.`"" @{ QB_ENCODER = "openh264" } | Out-Null
}
Stop-Job $focus; Remove-Job $focus -Force
$stolen = @(Grep (Join-Path $dir "focus.log") "STOLEN").Count
Say "focus: $stolen STOLEN samples"
New-Item -ItemType File (Join-Path $repo "tools\capture\STOP") -Force | Out-Null
Say "slot done; capture and OVRService stop requested"
