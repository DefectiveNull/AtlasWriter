# Installs AtlasWriter for the current user, without admin rights.
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Source <folder>]
#
# The source is this script's folder in a release archive, or ../build when run
# from a repository checkout.

param([string]$Source)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
if (-not $Source) {
    if (Test-Path (Join-Path $here 'AtlasWriter.exe')) {
        $Source = $here
    } else {
        $Source = Join-Path $here '..\build'
    }
}
if (-not (Test-Path (Join-Path $Source 'AtlasWriter.exe'))) {
    throw "AtlasWriter.exe was not found in '$Source'. Build the project or pass -Source."
}
$Source = (Resolve-Path $Source).Path

$target = Join-Path $env:LOCALAPPDATA 'Programs\AtlasWriter'
$targetExe = Join-Path $target 'AtlasWriter.exe'
if ($Source.TrimEnd('\') -ieq $target) {
    throw "The source is the install folder itself: '$target'."
}

$running = Get-Process AtlasWriter -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $targetExe }
if ($running) {
    throw 'AtlasWriter is running from the install folder. Close it and try again.'
}

# Only these go in: a build folder also holds CMake's output.
$required = 'AtlasWriter.exe', 'icon.svg', 'settings.example.ccl', 'assets'
$optional = 'LICENSE', 'README.md', 'licenses'
foreach ($name in $required) {
    if (-not (Test-Path (Join-Path $Source $name))) {
        throw "'$name' is missing from '$Source'."
    }
}

New-Item -ItemType Directory -Force $target | Out-Null
foreach ($name in $required + $optional) {
    $from = Join-Path $Source $name
    $to = Join-Path $target $name
    if (Test-Path $to) {
        Remove-Item $to -Recurse -Force
    }
    if (Test-Path $from) {
        Copy-Item $from $to -Recurse
    }
}
Copy-Item (Join-Path $here 'uninstall.ps1') $target -Force
Set-Content -Path (Join-Path $target 'installed.marker') -Value 'AtlasWriter' -Encoding ascii

# Start menu entry, which is what makes it searchable.
$shortcutPath = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\AtlasWriter.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $targetExe
$shortcut.WorkingDirectory = $target
$shortcut.IconLocation = "$targetExe,0"
$shortcut.Description = 'A minimalist text editor for the thinking mind'
$shortcut.Save()

# Entry in Settings > Apps.
$version = (& $targetExe --version | Out-String).Trim()
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AtlasWriter'
New-Item -Path $key -Force | Out-Null
$uninstall = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $target 'uninstall.ps1') + '"'
Set-ItemProperty -Path $key -Name DisplayName -Value 'AtlasWriter'
Set-ItemProperty -Path $key -Name DisplayVersion -Value $version
Set-ItemProperty -Path $key -Name Publisher -Value 'DefectiveNull'
Set-ItemProperty -Path $key -Name InstallLocation -Value $target
Set-ItemProperty -Path $key -Name DisplayIcon -Value $targetExe
Set-ItemProperty -Path $key -Name UninstallString -Value $uninstall
Set-ItemProperty -Path $key -Name NoModify -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name NoRepair -Value 1 -Type DWord

Write-Output "AtlasWriter $version installed to $target"
