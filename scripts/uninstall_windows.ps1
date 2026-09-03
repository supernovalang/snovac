# uninstall_windows.ps1 — Native Windows Uninstaller for Snovalang (snovac)

param(
    [string]$Prefix = "",
    [string]$BinDir = "",
    [string]$LibDir = "",
    [string]$IncDir = ""
)

$ErrorActionPreference = "SilentlyContinue"

if ([string]::IsNullOrEmpty($Prefix)) {
    if (-not [string]::IsNullOrEmpty($env:SNOVA_INSTALL_DIR)) {
        $Prefix = $env:SNOVA_INSTALL_DIR
    } elseif (-not [string]::IsNullOrEmpty($env:USERPROFILE)) {
        $Prefix = Join-Path $env:USERPROFILE ".snova"
    } else {
        $Prefix = "C:\Program Files\Snova"
    }
}
$Prefix = [System.IO.Path]::GetFullPath($Prefix)

if ([string]::IsNullOrEmpty($BinDir)) { $BinDir = Join-Path $Prefix "bin" }
if ([string]::IsNullOrEmpty($LibDir)) { $LibDir = Join-Path $Prefix "lib" }
if ([string]::IsNullOrEmpty($IncDir)) { $IncDir = Join-Path $Prefix "include" }

Write-Host "Uninstalling Snovalang (snovac) from $Prefix..." -ForegroundColor Yellow

Remove-Item -Path (Join-Path $BinDir "snovac.exe") -Force
Remove-Item -Path (Join-Path $LibDir "libsnovart.a") -Force
Remove-Item -Path (Join-Path $IncDir "*.h") -Force
Remove-Item -Path $Prefix -Recurse -Force
Remove-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\snovac" -Force -Recurse

Write-Host "[✓] Removed snovac from $BinDir/snovac.exe (and runtime lib/headers)" -ForegroundColor Green
Write-Host "Note: PATH entries in User environment and PowerShell profile are left untouched." -ForegroundColor Gray
