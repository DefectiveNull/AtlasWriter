# Removes AtlasWriter for the current user. Keeps Documents\AtlasWriter, which
# holds the user's settings.
#
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1

$ErrorActionPreference = 'Stop'

$target = Join-Path $env:LOCALAPPDATA 'Programs\AtlasWriter'
$targetExe = Join-Path $target 'AtlasWriter.exe'

$running = Get-Process AtlasWriter -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $targetExe }
if ($running) {
    throw 'AtlasWriter is running. Close it and try again.'
}

$shortcutPath = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\AtlasWriter.lnk'
if (Test-Path $shortcutPath) {
    Remove-Item $shortcutPath -Force
}
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AtlasWriter'
if (Test-Path $key) {
    Remove-Item $key -Recurse -Force
}

# This script lives in the folder it removes, so step out of it first.
Set-Location $env:TEMP
if (Test-Path $target) {
    Remove-Item $target -Recurse -Force
}

Write-Output 'AtlasWriter was removed. Your settings in Documents\AtlasWriter were kept.'
