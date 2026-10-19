$root = 'D:\Projects\cpp\embed-ai-assistant\components\esp-opus'
$removed = 0

Get-ChildItem -Path $root -Directory -Recurse |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object {
        (Get-ChildItem -LiteralPath $_.FullName -Force -File -Recurse).Count -eq 0
    } |
    ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Force -Recurse
        Write-Host "  removed: $($_.FullName.Replace($root + '\', ''))"
        $removed++
    }

Write-Host "`nDone. Removed $removed empty directories."
