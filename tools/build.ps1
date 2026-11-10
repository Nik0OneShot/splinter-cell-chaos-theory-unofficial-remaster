# Builds the SCCT Unofficial Remaster and assembles a release folder + zip in dist\.
# Usage (from the repository root):  powershell -ExecutionPolicy Bypass -File tools\build.ps1
param(
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# --- Locate CMake (Visual Studio's bundled copy works fine) -------------------------------
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path $cmake)) { throw "CMake not found. Install Visual Studio with the C++ workload." }

# --- Build ---------------------------------------------------------------------------------
& $cmake -S . -B build -A Win32 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $cmake --build build --config $Configuration --target d3d9 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$version = (Select-String -Path src\version.h -Pattern 'SCCTR_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
$name = "SCCT-Unofficial-Remaster-v$version"
$dist = Join-Path $root "dist\$name"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force (Join-Path $dist "System") | Out-Null

# --- Our files ------------------------------------------------------------------------------
Copy-Item "build\$Configuration\d3d9.dll" (Join-Path $dist "System\d3d9.dll")
Copy-Item "package\System\*" (Join-Path $dist "System") -Recurse
Copy-Item "tools\install.ps1", "tools\Install.bat" $dist
Copy-Item "package\Mods" (Join-Path $dist "Mods") -Recurse
Copy-Item "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "CHANGELOG.md" $dist

# --- The CLUSTERFUCK award ----------------------------------------------------------------------
# Packed from references\niko_plush (local only: a glTF model and a meow sound; see tools\pack_plush.py). Without it
# the package has no plush and the game runs without one.
$plushSrc = Join-Path $root "references\niko_plush"
if (Test-Path $plushSrc) {
    & python -I (Join-Path $root "tools\pack_plush.py") $plushSrc (Join-Path $dist "System\SCCTRemaster_Award.pak") | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Warning "Packing the award failed: the package has none." }
} else {
    Write-Host "No references\niko_plush: the package has no award."
}

# --- DXVK (Direct3D 9 on Vulkan, zlib license): loaded through [Graphics] D3D9Chain -----------
Copy-Item "package\dxvk\d3d9_dxvk.dll", "package\dxvk\dxvk.conf" (Join-Path $dist "System")
Copy-Item "package\dxvk\DXVK-LICENSE.txt" $dist

# --- Bundled third-party components (from the reference packages) ---------------------------
# They come from ThirteenAG's Widescreen Fix release, which isn't in the repository: download
# SplinterCellChaosTheory.WidescreenFix.zip into references\ first.
$wsfix = Join-Path $root "references\SplinterCellChaosTheory.WidescreenFix.zip"
if (-not (Test-Path $wsfix)) {
    throw "Missing $wsfix. Download it from https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/scct (it provides DSOAL and Xidi)."
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($wsfix)
try {
    foreach ($entry in $zip.Entries) {
        # DSOAL (dsound.dll + OpenAL Soft) for EAX, Xidi for controllers.
        if ($entry.FullName -in @("System/dsound.dll", "System/dsoal-aldrv.dll", "System/Xidi.32.dll")) {
            $target = Join-Path $dist ($entry.FullName -replace "/", "\")
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    }
} finally { $zip.Dispose() }

# --- Zip ------------------------------------------------------------------------------------
$zipPath = Join-Path $root "dist\$name.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zipPath
Write-Host ""
Write-Host "Built $name"
Write-Host "  folder: $dist"
Write-Host "  zip:    $zipPath"
