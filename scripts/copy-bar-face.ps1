# Copy embed-ai-assistant into bar-face without build artifacts / .git
param(
    [string]$Source = "D:\Projects\cpp\embed-ai-assistant",
    [string]$Dest = (Join-Path $PSScriptRoot "..\bar-face")
)

$ErrorActionPreference = "Stop"
$Dest = [System.IO.Path]::GetFullPath($Dest)

if (-not (Test-Path $Source)) {
    Write-Error "Source not found: $Source"
}

Write-Host "Copying $Source -> $Dest"

if (Test-Path $Dest) {
    Write-Host "Removing existing $Dest"
    Remove-Item -Recurse -Force $Dest
}

New-Item -ItemType Directory -Path $Dest | Out-Null

$excludeDirs = @("build", ".git", ".cache", "managed_components")
$robolog = Join-Path $env:TEMP "bar-face-copy.log"

$xd = ($excludeDirs | ForEach-Object { "/XD"; $_ })
& robocopy $Source $Dest /E /NFL /NDL /NJH /NJS /nc /ns /np @xd /XF "*.pyc" | Out-Host
# robocopy exit codes 0-7 are success
if ($LASTEXITCODE -ge 8) {
    Write-Error "robocopy failed with exit $LASTEXITCODE (see $robolog)"
}

Write-Host "Done. Next: rename project in CMakeLists and flash with idf.py"
exit 0
