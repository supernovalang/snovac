# build_installer.ps1 — Build Native Windows Installers for snovac

param(
    [string]$Version = "0.0.1-p1"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if ([string]::IsNullOrEmpty($ScriptDir)) { $ScriptDir = (Get-Location).Path }
$RootDir = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\.."))
$BuildDir = Join-Path $RootDir "build"
$OutDir = Join-Path $BuildDir "installer"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Building Native Windows Installers for snovac v$Version" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Package Portable ZIP Archive
$StageDir = Join-Path $OutDir "stage"
Remove-Item -Path $StageDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path (Join-Path $StageDir "bin") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $StageDir "lib") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $StageDir "include") -Force | Out-Null

Copy-Item -Path (Join-Path $BuildDir "snovac.exe") -Destination (Join-Path $StageDir "bin\snovac.exe") -Force
Copy-Item -Path (Join-Path $BuildDir "libsnovart.a") -Destination (Join-Path $StageDir "lib\libsnovart.a") -Force
Get-ChildItem -Path $RootDir -Filter "*.h" -File | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination (Join-Path $StageDir "include") -Force
}
Copy-Item -Path (Join-Path $RootDir "README.md") -Destination $StageDir -Force
Copy-Item -Path (Join-Path $RootDir "install.ps1") -Destination $StageDir -Force
Copy-Item -Path (Join-Path $RootDir "scripts\install_windows.ps1") -Destination (Join-Path $StageDir "install_windows.ps1") -Force

$batContent = "@echo off`r`necho Installing Snovalang Compiler (snovac)...`r`npowershell -ExecutionPolicy Bypass -NoProfile -File `"%~dp0install_windows.ps1`"`r`npause"
Set-Content -Path (Join-Path $StageDir "install.bat") -Value $batContent -Encoding ASCII

$ZipFile = Join-Path $OutDir "snovac-windows-x86_64.zip"
Remove-Item -Path $ZipFile -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipFile -Force
Write-Host "[✓] Created portable ZIP installer: $ZipFile" -ForegroundColor Green

# 2. Try Inno Setup (.exe installer)
$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $possiblePaths = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe",
        "C:\Program Files (x86)\Inno Setup 5\ISCC.exe"
    )
    foreach ($p in $possiblePaths) {
        if (Test-Path $p) { $iscc = $p; break }
    }
}

if ($iscc) {
    Write-Host "==> Building Inno Setup Windows Installer..." -ForegroundColor Cyan
    $issFile = Join-Path $ScriptDir "snovac.iss"
    & $iscc $issFile
    Write-Host "[✓] Created native Windows Setup EXE in $OutDir" -ForegroundColor Green
}
if (-not $iscc) {
    Write-Host "[*] Inno Setup compiler (ISCC.exe) not found. Skipping .exe setup generation." -ForegroundColor Gray
}

# 3. Try WiX Toolset (.msi installer)
$candle = (Get-Command candle -ErrorAction SilentlyContinue).Source
$light = (Get-Command light -ErrorAction SilentlyContinue).Source
$wix = (Get-Command wix -ErrorAction SilentlyContinue).Source
$msiFile = Join-Path $OutDir "snovac-windows-x86_64.msi"

if ($wix) {
    Write-Host "==> Building WiX MSI Package..." -ForegroundColor Cyan
    $wxsFile = Join-Path $ScriptDir "snovac.wxs"
    & $wix build $wxsFile -o $msiFile
    Write-Host "[✓] Created native Windows MSI installer: $msiFile" -ForegroundColor Green
}
if (-not $wix -and ($candle -and $light)) {
    Write-Host "==> Building WiX MSI Package..." -ForegroundColor Cyan
    Push-Location $ScriptDir
    try {
        & $candle snovac.wxs -out "$OutDir\snovac.wixobj"
        & $light "$OutDir\snovac.wixobj" -out $msiFile
        Write-Host "[✓] Created native Windows MSI installer: $msiFile" -ForegroundColor Green
    } finally {
        Pop-Location
    }
}
if (-not $wix -and (-not $candle -or -not $light)) {
    Write-Host "[*] WiX Toolset (candle/light/wix) not found. Skipping .msi generation." -ForegroundColor Gray
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host " Windows Packaging Completed!" -ForegroundColor Green
Write-Host " Artifacts available in: $OutDir" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Green
