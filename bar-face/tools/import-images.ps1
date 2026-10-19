# Copies PNG images from a source folder into the project assets folder.
#
# Usage:
#   .\tools\import-images.ps1                            # default source
#   .\tools\import-images.ps1 -Source D:\Projects\youtube
#   .\tools\import-images.ps1 -Source C:\pics -Dest C:\other\assets
#
# After copying, rebuild the firmware: CMake re-scans the assets folder on
# every build and embeds each PNG into flash. The startup screen uses the
# image named "barmen-idle" (barmen-idle.png), if present.

param(
    [string]$Source = "D:\Projects\youtube",
    [string]$Dest   = "$PSScriptRoot\..\assets"
)

if (-not (Test-Path -LiteralPath $Source)) {
    Write-Error "Source folder not found: $Source"
    exit 1
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

$copied = 0
Get-ChildItem -LiteralPath $Source -Filter *.png -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $Dest -Force
    Write-Host "copied: $($_.Name)"
    $copied++
}

Write-Host ""
Write-Host "done: $copied image(s) in $Dest"
Write-Host "rebuild the firmware so CMake embeds the new images."

# Bump the display CMakeLists.txt mtime so the next `idf.py build` sees
# the new file list (CONFIGURE_DEPENDS is disabled in the component's
# CMakeLists because ESP-IDF includes it in script mode).
$marker = Join-Path $Dest '..\components\display\CMakeLists.txt'
$marker = [System.IO.Path]::GetFullPath($marker)
if (Test-Path -LiteralPath $marker) {
    (Get-Item -LiteralPath $marker).LastWriteTime = Get-Date
    Write-Host "touched: $marker (forces reconfigure on next build)"
}
