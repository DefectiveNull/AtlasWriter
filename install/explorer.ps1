# Shows a Markdown file's ```meta block (title, tags, author, description,
# dates) in File Explorer: the details pane, the Properties dialog, columns
# and search.
#
#   powershell -ExecutionPolicy Bypass -File explorer.ps1 -Register
#   powershell -ExecutionPolicy Bypass -File explorer.ps1 -Unregister
#
# Windows only reads property handlers registered for the whole machine, so
# this asks for admin rights once (a UAC prompt) for that part. Which
# properties Explorer shows for .md files is set for the current user.
# install.ps1 and uninstall.ps1 run this; after a self-update from 1.1.0, run
# it from the install folder to turn the integration on.

param(
    [switch]$Register,
    [switch]$Unregister,
    [switch]$Machine,   # internal: the elevated half
    [string]$Dll
)

$ErrorActionPreference = 'Stop'

$clsid = '{B7A1B14C-EA09-4CBC-9930-2E4F1B4D7524}' # matches src/explorer/PropertyHandler.cpp
$extensions = '.md', '.markdown'
$handlers = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers'
$backupKey = 'HKLM:\SOFTWARE\AtlasWriter\PreviousPropertyHandlers'
$clsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"

if ($Register -eq $Unregister) {
    throw 'Pass either -Register or -Unregister.'
}
if (-not $Dll) {
    $Dll = Join-Path $PSScriptRoot 'AtlasWriterProps.dll'
}

Add-Type -Namespace AtlasWriter -Name Native -MemberDefinition @'
[DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool MoveFileEx(string existing, string replacement, int flags);
'@

function Notify-Shell {
    [AtlasWriter.Native]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero) # SHCNE_ASSOCCHANGED
}

# --- The elevated half: machine-wide registration ----------------------------

if ($Machine) {
    if ($Register) {
        if (-not (Test-Path $Dll)) {
            throw "'$Dll' was not found."
        }
        New-Item -Path "$clsidKey\InprocServer32" -Force | Out-Null
        Set-Item -Path $clsidKey -Value 'AtlasWriter Markdown properties'
        Set-Item -Path "$clsidKey\InprocServer32" -Value $Dll
        Set-ItemProperty -Path "$clsidKey\InprocServer32" -Name ThreadingModel -Value 'Both'

        New-Item -Path $backupKey -Force | Out-Null
        foreach ($ext in $extensions) {
            $key = "$handlers\$ext"
            $previous = $null
            if (Test-Path $key) {
                $previous = (Get-Item $key).GetValue('')
            }
            # Keep another program's handler, to put back on unregister.
            if ($previous -and $previous -ne $clsid) {
                Set-ItemProperty -Path $backupKey -Name $ext -Value $previous
            }
            New-Item -Path $key -Force | Out-Null
            Set-Item -Path $key -Value $clsid
        }
    } else {
        foreach ($ext in $extensions) {
            $key = "$handlers\$ext"
            if ((Test-Path $key) -and (Get-Item $key).GetValue('') -eq $clsid) {
                $previous = $null
                if (Test-Path $backupKey) {
                    $previous = (Get-Item $backupKey).GetValue($ext)
                }
                if ($previous) {
                    Set-Item -Path $key -Value $previous
                } else {
                    Remove-Item $key -Recurse -Force
                }
            }
        }
        if (Test-Path $clsidKey) {
            Remove-Item $clsidKey -Recurse -Force
        }
        if (Test-Path 'HKLM:\SOFTWARE\AtlasWriter') {
            Remove-Item 'HKLM:\SOFTWARE\AtlasWriter' -Recurse -Force
        }

        # Explorer may still have the DLL loaded. A loaded DLL can be moved
        # but not deleted, so move it out of the install folder (letting
        # uninstall.ps1 remove the folder) and delete it at the next restart.
        # Copies set aside by reinstalls and updates go the same way.
        $folder = Split-Path $Dll
        if (Test-Path $folder) {
            foreach ($file in Get-ChildItem $folder -Recurse -File -Filter 'AtlasWriterProps*.dll*') {
                try {
                    Remove-Item $file.FullName -Force
                } catch {
                    $aside = Join-Path $env:windir "Temp\AtlasWriterProps-$([guid]::NewGuid()).dll"
                    Move-Item $file.FullName $aside -Force
                    [AtlasWriter.Native]::MoveFileEx($aside, $null, 4) | Out-Null # MOVEFILE_DELAY_UNTIL_REBOOT
                }
            }
        }
    }
    Notify-Shell
    exit 0
}

# --- The current user's half: which properties Explorer shows ---------------

$details = @{
    PreviewDetails = 'prop:System.Title;System.Keywords;System.Author;System.Comment;System.Document.DateCreated;System.Document.DateSaved;System.DateModified;System.Size'
    FullDetails    = 'prop:System.PropGroup.Description;System.Title;System.Keywords;System.Comment;System.PropGroup.Origin;System.Author;System.Document.DateCreated;System.Document.DateSaved;System.PropGroup.FileSystem;System.ItemNameDisplay;System.ItemType;System.ItemFolderPathDisplay;System.Size;System.DateCreated;System.DateModified'
    InfoTip        = 'prop:System.Title;System.Keywords;System.Author;System.DateModified'
}

foreach ($ext in $extensions) {
    $key = "HKCU:\Software\Classes\SystemFileAssociations\$ext"
    if ($Register) {
        New-Item -Path $key -Force | Out-Null
        foreach ($name in $details.Keys) {
            Set-ItemProperty -Path $key -Name $name -Value $details[$name]
        }
        Set-ItemProperty -Path $key -Name 'AtlasWriter' -Value 1 -Type DWord # ours, for unregister
    } elseif ((Test-Path $key) -and (Get-Item $key).GetValue('AtlasWriter') -eq 1) {
        foreach ($name in @($details.Keys) + 'AtlasWriter') {
            Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
        }
        $item = Get-Item $key
        if ($item.ValueCount -eq 0 -and $item.SubKeyCount -eq 0) {
            Remove-Item $key -Force
        }
    }
}

# The machine-wide half, elevated. Use 64-bit PowerShell so the registry
# isn't redirected to WOW6432Node when this runs from a 32-bit one.
$system = 'System32'
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $system = 'Sysnative'
}
$powershell = Join-Path $env:windir "$system\WindowsPowerShell\v1.0\powershell.exe"
$action = if ($Register) { '-Register' } else { '-Unregister' }
$arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" $action -Machine -Dll `"$Dll`""
try {
    $process = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -Wait -PassThru -WindowStyle Hidden
} catch {
    Write-Warning 'Admin rights were declined, so File Explorer integration was skipped. Run explorer.ps1 again to retry.'
    Notify-Shell
    exit 1
}
Notify-Shell
if ($process.ExitCode -ne 0) {
    Write-Warning "The admin step failed (exit code $($process.ExitCode))."
    exit 1
}
if ($Register) {
    Write-Output 'File Explorer now shows Markdown metadata. Already-open Explorer windows may need reopening.'
} else {
    Write-Output 'File Explorer integration removed.'
}
