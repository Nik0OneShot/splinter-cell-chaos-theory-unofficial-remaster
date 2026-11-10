# Installs the SCCT Unofficial Remaster into a Splinter Cell Chaos Theory folder.
# Conflicting mods (FusionFix/WidescreenFix, Ultimate ASI Loader, SCFix, old wined3d fix, ALchemy)
# are moved to System\_backup_before_remaster\<date> - nothing is deleted.
#
# Usage:  powershell -ExecutionPolicy Bypass -File install.ps1 [-GamePath "D:\SteamLibrary\steamapps\common\Splintercell Chaos Theory"]
param(
    [string]$GamePath = ""
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

function Find-Game {
    $candidates = @()
    # Steam library folders
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        $libs = @($steam)
        if (Test-Path $vdf) {
            $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"').Matches | ForEach-Object { $_.Groups[1].Value -replace "\\\\", "\" }
        }
        foreach ($lib in $libs) { $candidates += Join-Path $lib "steamapps\common\Splintercell Chaos Theory" }
    }
    $candidates += (Split-Path -Parent $here)
    foreach ($c in $candidates) { if (Test-Path (Join-Path $c "System\splintercell3.exe")) { return $c } }
    return $null
}

if (-not $GamePath) { $GamePath = Find-Game }
if (-not $GamePath -or -not (Test-Path (Join-Path $GamePath "System\splintercell3.exe"))) {
    $GamePath = Read-Host "Splinter Cell Chaos Theory folder (the one that contains 'System')"
}
$sys = Join-Path $GamePath "System"
if (-not (Test-Path (Join-Path $sys "splintercell3.exe"))) { throw "splintercell3.exe not found in $sys" }
Write-Host "Installing into: $GamePath"

$backup = Join-Path $sys ("_backup_before_remaster\" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss"))
function Backup([string]$rel) {
    $src = Join-Path $sys $rel
    if (Test-Path $src) {
        $dst = Join-Path $backup $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
        Move-Item -Force $src $dst
        Write-Host "  backed up $rel"
    }
}

# Old wined3d AMD thermal fix: keep it available as an optional D3D9Chain target.
$oldD3d9 = Join-Path $sys "d3d9.dll"
if (Test-Path $oldD3d9) {
    $isWined3d = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($oldD3d9)).Contains("wined3d.dll")
    $isOurs = (Get-Item $oldD3d9).VersionInfo.ProductName -like "*Unofficial Remaster*"
    if ($isWined3d) {
        Copy-Item -Force $oldD3d9 (Join-Path $sys "d3d9_wined3d.dll")
        Write-Host "  kept the old wined3d AMD fix as d3d9_wined3d.dll (see [Graphics] D3D9Chain)"
    }
    if (-not $isOurs) { Backup "d3d9.dll" }
}

# Mods the remaster replaces.
foreach ($f in @("dinput8.dll", "scfix.ini", "msacm32.dll", "msvfw32.dll", "dsound.dll", "dsound.ini", "dsoundlog.txt",
                 "dsoal-aldrv.dll", "Xidi.32.dll","scripts\SplinterCellChaosTheory.WidescreenFix.asi", "scripts\SplinterCellChaosTheory.WidescreenFix.ini",
                 "scripts\SplinterCellChaosTheory.FusionFix.asi", "scripts\SplinterCellChaosTheory.FusionFix.ini")) {
    Backup $f
}
# Keep the user's own settings if they already have them.
foreach ($f in @("SCCTRemaster.ini", "SCCTRemaster_Guards.ini", "alsoft.ini", "Xidi.ini", "JoysticksDefault.ini", "dxvk.conf", "d3d9_dxvk.dll")) {
    $src = Join-Path $sys $f
    if (Test-Path $src) {
        New-Item -ItemType Directory -Force $backup | Out-Null
        Copy-Item -Force $src (Join-Path $backup $f)
    }
}
# Settings files the user may have edited: keep theirs, write the new defaults next to them.
$userConfigs = @("SCCTRemaster.ini", "SCCTRemaster_Guards.ini")

Get-ChildItem (Join-Path $here "System") | ForEach-Object {
    if ($userConfigs -contains $_.Name -and (Test-Path (Join-Path $sys $_.Name))) {
        $defaults = $_.BaseName + ".default" + $_.Extension
        Copy-Item -Force $_.FullName (Join-Path $sys $defaults)
        Write-Host "  kept your $($_.Name) (new defaults written to $defaults)"
    } else {
        Copy-Item -Force $_.FullName (Join-Path $sys $_.Name)
    }
}
# Mods folder: the readme and the example mod (the player's own mods are left alone).
$modsSrc = Join-Path $here "Mods"
if (Test-Path $modsSrc) {
    $modsDst = Join-Path $GamePath "Mods"
    New-Item -ItemType Directory -Force $modsDst | Out-Null
    Copy-Item -Force (Join-Path $modsSrc "README.txt"), (Join-Path $modsSrc "SCRIPTING.txt"), (Join-Path $modsSrc "TEXTURES.txt"), (Join-Path $modsSrc "LEVELS.txt") $modsDst
    Copy-Item -Force -Recurse (Join-Path $modsSrc "_Example") $modsDst
}
Write-Host ""
Write-Host "Done. Settings: System\SCCTRemaster.ini   Log: System\SCCTRemaster.log"
Write-Host "Previous files are in: $backup"
