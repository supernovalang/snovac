# ==============================================================================
# Snovalang Compiler (snovac) Universal Windows One-Line Installer
# Usage: irm https://raw.githubusercontent.com/supernovalang/snovac/master/install.ps1 | iex
# Or:    powershell -ExecutionPolicy Bypass -File install.ps1
# ==============================================================================

$ErrorActionPreference = "Stop"

$Repo = "supernovalang/snovac"
$Version = "0.0.1-p1"
$InstallPrefix = if (-not [string]::IsNullOrEmpty($env:SNOVA_INSTALL_DIR)) {
    $env:SNOVA_INSTALL_DIR
} else {
    Join-Path $env:USERPROFILE ".snova"
}
$InstallPrefix = [System.IO.Path]::GetFullPath($InstallPrefix)
$BinDir = [System.IO.Path]::GetFullPath((Join-Path $InstallPrefix "bin"))
$LibDir = [System.IO.Path]::GetFullPath((Join-Path $InstallPrefix "lib"))
$IncDir = [System.IO.Path]::GetFullPath((Join-Path $InstallPrefix "include"))

Write-Host @"
  ____                                         
 / ___| _ __   _____   ____ _ _ __   ___ _ __  
 \___ \| '_ \ / _ \ \ / / _` | '_ \ / _ \ '__| 
  ___) | | | | (_) \ V / (_| | | | |  __/ |    
 |____/|_| |_|\___/ \_/ \__,_|_| |_|\___|_|    
           Snovalang Compiler (snovac)
"@ -ForegroundColor Cyan

# 1. Detect Architecture
$arch = switch ($env:PROCESSOR_ARCHITECTURE) {
    "AMD64" { "x86_64" }
    "ARM64" { "aarch64" }
    default { "x86_64" }
}

$Platform = "windows-$arch"
$ZipName = "snovac-$Platform.zip"
$DownloadUrl = "https://github.com/$Repo/releases/latest/download/$ZipName"

Write-Host "==> Detected target: $Platform" -ForegroundColor Green

$TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("snovac-install-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TempDir -Force | Out-Null

try {
    $Installed = $false
    # 2. Try downloading pre-built zip release
    Write-Host "==> Checking release package $ZipName..." -ForegroundColor Cyan
    $ZipPath = Join-Path $TempDir $ZipName
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing -ErrorAction Stop
        Write-Host "[✓] Downloaded release archive." -ForegroundColor Green
        Expand-Archive -Path $ZipPath -DestinationPath (Join-Path $TempDir "extracted") -Force
        
        $binSource = Join-Path $TempDir "extracted\snovac.exe"
        if (Test-Path $binSource) {
            New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
            Copy-Item -Path $binSource -Destination (Join-Path $BinDir "snovac.exe") -Force
            $Installed = $true
        }
    } catch {
        Write-Host "Release archive not yet published on GitHub Releases. Falling back to local source build..." -ForegroundColor Yellow
    }

    # 3. Source build fallback
    if (-not $Installed) {
        $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
        $SourceDir = $ScriptDir
        if ([string]::IsNullOrEmpty($SourceDir) -or -not (Test-Path (Join-Path $SourceDir "Makefile"))) {
            $SourceDir = (Get-Location).Path
        }

        if (Test-Path (Join-Path $SourceDir "Makefile")) {
            Write-Host "==> Compiling snovac from source..." -ForegroundColor Cyan
            Push-Location $SourceDir
            try {
                if (Get-Command make -ErrorAction SilentlyContinue) {
                    make
                } elseif (Get-Command gcc -ErrorAction SilentlyContinue) {
                    gcc -std=c11 -O2 -g -pthread -o build/snovac.exe *.c
                } else {
                    Write-Error "GCC or Make is required to compile snovac from source on Windows. Install MinGW-w64 or use a pre-built binary."
                }
                
                # Execute native install script
                & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $SourceDir "scripts\install_windows.ps1") -Prefix "$InstallPrefix" -BinDir "$BinDir" -LibDir "$LibDir" -IncDir "$IncDir"
                $Installed = $true
            } finally {
                Pop-Location
            }
        } else {
            # Try git clone
            if (Get-Command git -ErrorAction SilentlyContinue) {
                Write-Host "==> Cloning $Repo repository..." -ForegroundColor Cyan
                $CloneDir = Join-Path $TempDir "snovac-repo"
                git clone --depth 1 "https://github.com/$Repo.git" $CloneDir
                Push-Location $CloneDir
                try {
                    make
                    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $CloneDir "scripts\install_windows.ps1") -Prefix "$InstallPrefix"
                    $Installed = $true
                } finally {
                    Pop-Location
                }
            } else {
                Write-Error "Neither prebuilt release nor git/make build environment was found."
            }
        }
    }

    if ($Installed) {
        Write-Host ""
        Write-Host "Snovalang Compiler (snovac) was successfully installed!" -ForegroundColor Green
        Write-Host "Run 'snovac --version' or 'snovac --target-info' to get started." -ForegroundColor Cyan
    }
} finally {
    Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
}
