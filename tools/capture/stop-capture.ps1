# Signals the elevated capture started by start-elevated.ps1 to stop.
New-Item -ItemType File -Force (Join-Path $PSScriptRoot "STOP") | Out-Null
