# purr.ps1 — PURR OS build system launcher (PowerShell wrapper)
#
# Usage:
#   .\purr.ps1                         # interactive tool picker
#   .\purr.ps1 purrstrap               # purrstrap interactive UI
#   .\purr.ps1 modulestrap             # modulestrap interactive UI
#   .\purr.ps1 catstrap                # catstrap interactive UI
#   .\purr.ps1 purrstrap build cyd     # pass-through CLI

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# Find Python
$Python = $null
foreach ($candidate in @("python3", "python", "py")) {
    if (Get-Command $candidate -ErrorAction SilentlyContinue) {
        $Python = $candidate
        break
    }
}

if (-not $Python) {
    Write-Error "[err] Python not found. Install Python 3.8+ and try again."
    exit 1
}

& $Python "$ScriptDir\purr.py" @args
exit $LASTEXITCODE
