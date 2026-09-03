# install_windows.ps1 ? Native Windows Installer for Snovalang (snovac)

param(
    [string]$Prefix = "",
    [string]$BinDir = "",
    [string]$LibDir = "",
    [string]$IncDir = "",
    [string]$Bin = "",
    [string]$LibRt = "",
    [string]$Version = "0.0.1-p1"
)

$ErrorActionPreference = "Stop"

# 1. Resolve Canonical Paths
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

$BinDir = [System.IO.Path]::GetFullPath($BinDir)
$LibDir = [System.IO.Path]::GetFullPath($LibDir)
$IncDir = [System.IO.Path]::GetFullPath($IncDir)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if ([string]::IsNullOrEmpty($ScriptDir)) { $ScriptDir = (Get-Location).Path }
$RootDir = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir ".."))

if ([string]::IsNullOrEmpty($Bin)) {
    $Bin = Join-Path $RootDir "build\snovac.exe"
    if (-not (Test-Path $Bin)) {
        $Bin = Join-Path $RootDir "snovac.exe"
    }
}
if ([string]::IsNullOrEmpty($LibRt)) {
    $LibRt = Join-Path $RootDir "build\libsnovart.a"
    if (-not (Test-Path $LibRt)) {
        $LibRt = Join-Path $RootDir "libsnovart.a"
    }
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Installing Snovalang Compiler (snovac) for Windows" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Target Prefix : $Prefix"
Write-Host " Binary Dir    : $BinDir"
Write-Host " Library Dir   : $LibDir"
Write-Host " Include Dir   : $IncDir"
Write-Host ""

# 2. Create Target Directories
New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
New-Item -ItemType Directory -Path $LibDir -Force | Out-Null
New-Item -ItemType Directory -Path $IncDir -Force | Out-Null

# 3. Copy Binary and Libraries
if (Test-Path $Bin) {
    Copy-Item -Path $Bin -Destination (Join-Path $BinDir "sncli.exe") -Force
    Copy-Item -Path $Bin -Destination (Join-Path $BinDir "snovac.exe") -Force
    Write-Host "[OK] Installed sncli CLI to $(Join-Path $BinDir 'sncli.exe')" -ForegroundColor Green
    Write-Host "[OK] Updated snovac CLI to $(Join-Path $BinDir 'snovac.exe')" -ForegroundColor Green
} else {
    Write-Error "Could not find binary at '$Bin'. Please build sncli first."
}

if (Test-Path $LibRt) {
    Copy-Item -Path $LibRt -Destination (Join-Path $LibDir "libsnovart.a") -Force
    Write-Host "[OK] Installed runtime library to $(Join-Path $LibDir 'libsnovart.a')" -ForegroundColor Green
}

# 4. Copy Header Files
$headers = Get-ChildItem -Path $RootDir -Filter "*.h" -File
foreach ($h in $headers) {
    Copy-Item -Path $h.FullName -Destination $IncDir -Force
}
Write-Host "[OK] Installed $($headers.Count) C header files to $IncDir" -ForegroundColor Green

# 5. Copy snova-std if present
$stdSrc = Join-Path $RootDir "..\snova-std\src"
$stdTarget = Join-Path $env:USERPROFILE ".snovalang\std\src"
if (Test-Path $stdSrc) {
    $stdTarget = [System.IO.Path]::GetFullPath($stdTarget)
    New-Item -ItemType Directory -Path $stdTarget -Force | Out-Null
    Copy-Item -Path "$stdSrc\*" -Destination $stdTarget -Recurse -Force
    Write-Host "[OK] Installed snova-std to $stdTarget" -ForegroundColor Green
}

# 6. Configure Environment Variables and PATH (Canonical)
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ([string]::IsNullOrEmpty($userPath)) { $userPath = "" }
$pathEntries = @()
foreach ($p in $userPath.Split(";")) {
    if (-not [string]::IsNullOrWhiteSpace($p)) {
        $pathEntries += [System.IO.Path]::GetFullPath($p).TrimEnd([char[]]@('\', '/'))
    }
}

$canonicalBinDir = $BinDir.TrimEnd([char[]]@('\', '/'))
if ($pathEntries -notcontains $canonicalBinDir) {
    $newPath = if ($userPath -eq "") { $canonicalBinDir } else { "$userPath;$canonicalBinDir" }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Write-Host "[OK] Added '$canonicalBinDir' to User PATH environment variable." -ForegroundColor Green
} else {
    Write-Host "[*] User PATH already contains '$canonicalBinDir'" -ForegroundColor Gray
}

# Add to current session
$currentPathEntries = @()
foreach ($p in $env:Path.Split(";")) {
    if (-not [string]::IsNullOrWhiteSpace($p)) {
        $currentPathEntries += [System.IO.Path]::GetFullPath($p).TrimEnd([char[]]@('\', '/'))
    }
}
if ($currentPathEntries -notcontains $canonicalBinDir) {
    $env:Path = "$canonicalBinDir;$env:Path"
}

# 7. Wire into PowerShell Profile
try {
    if (-not (Test-Path -Path $PROFILE)) {
        New-Item -ItemType File -Path $PROFILE -Force | Out-Null
    }
    $profileContent = Get-Content -Path $PROFILE -Raw -ErrorAction SilentlyContinue
    if (-not $profileContent -or ($profileContent -notlike "*$canonicalBinDir*")) {
        $exportLine = "`$snovaBin = '$canonicalBinDir'; if (`$env:Path.Split(';') -notcontains `$snovaBin) { `$env:Path = `"`$snovaBin;`$env:Path`" }"
        Add-Content -Path $PROFILE -Value "`n# Snovalang Compiler PATH`n$exportLine"
        Write-Host "[OK] Updated PowerShell Profile: $PROFILE" -ForegroundColor Green
    }
} catch {
    $null = $_
}

# 8. Generate Uninstaller Script
$uninstallScript = Join-Path $Prefix "uninstall.ps1"
$lines = @(
    "# Snovalang Compiler Uninstaller for Windows",
    "param([switch]`$Quiet)",
    "`$ErrorActionPreference = 'SilentlyContinue'",
    "`$Prefix = '$Prefix'",
    "`$BinDir = '$BinDir'",
    "`$LibDir = '$LibDir'",
    "`$IncDir = '$IncDir'",
    "",
    "Remove-Item -Path (Join-Path `$BinDir 'snovac.exe') -Force",
    "Remove-Item -Path (Join-Path `$LibDir 'libsnovart.a') -Force",
    "Remove-Item -Path (Join-Path `$IncDir '*.h') -Force",
    "Remove-Item -Path `$Prefix -Recurse -Force",
    "Remove-Item -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\snovac' -Force -Recurse",
    "",
    "if (-not `$Quiet) {",
    "    Write-Host 'Snovalang (snovac) was successfully uninstalled.' -ForegroundColor Green",
    "}"
)
Set-Content -Path $uninstallScript -Value $lines -Force

# 9. Register in Windows Settings / Add/Remove Programs (Registry)
$uninstallRegKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\snovac"
try {
    if (-not (Test-Path $uninstallRegKey)) {
        New-Item -Path $uninstallRegKey -Force | Out-Null
    }
    Set-ItemProperty -Path $uninstallRegKey -Name "DisplayName" -Value "Snovalang Compiler (snovac)"
    Set-ItemProperty -Path $uninstallRegKey -Name "DisplayVersion" -Value $Version
    Set-ItemProperty -Path $uninstallRegKey -Name "Publisher" -Value "Snovalang Project"
    Set-ItemProperty -Path $uninstallRegKey -Name "InstallLocation" -Value $Prefix
    Set-ItemProperty -Path $uninstallRegKey -Name "UninstallString" -Value "powershell.exe -ExecutionPolicy Bypass -File `"$uninstallScript`""
    Set-ItemProperty -Path $uninstallRegKey -Name "QuietUninstallString" -Value "powershell.exe -ExecutionPolicy Bypass -File `"$uninstallScript`" -Quiet"
    Set-ItemProperty -Path $uninstallRegKey -Name "NoModify" -Value 1 -Type DWord
    Set-ItemProperty -Path $uninstallRegKey -Name "NoRepair" -Value 1 -Type DWord
    Write-Host "[OK] Registered snovac in Windows Installed Apps (Control Panel)" -ForegroundColor Green
} catch {
    $null = $_
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host " Snovalang Compiler (snovac) is ready to use!" -ForegroundColor Green
Write-Host " Try running: snovac --version" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Green
