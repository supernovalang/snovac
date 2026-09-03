# install_path.ps1 — persist BinDir on PATH for PowerShell (and future
# sessions via the User environment variable), on Windows.
#
# Called by `make install` (see Makefile) once the snovac binary has been
# copied into BinDir. Idempotent: re-running it after BinDir is already on
# PATH is a no-op.

param(
    [Parameter(Mandatory = $true)]
    [string]$BinDir
)

$ErrorActionPreference = "Stop"

# Persist to the User PATH so new terminals (cmd, PowerShell, Windows
# Terminal, etc.) pick it up without needing the profile script below.
$currentUserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ([string]::IsNullOrEmpty($currentUserPath)) {
    $currentUserPath = ""
}

if ($currentUserPath.Split(";") -notcontains $BinDir) {
    $newPath = if ($currentUserPath -eq "") { $BinDir } else { "$currentUserPath;$BinDir" }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Write-Host "Updated User PATH environment variable."
} else {
    Write-Host "User PATH already contains $BinDir"
}

# Make it available in the current PowerShell session immediately.
if ($env:Path.Split(";") -notcontains $BinDir) {
    $env:Path = "$BinDir;$env:Path"
}

# Also wire it into the PowerShell profile, so PATH is guaranteed even in
# contexts that don't inherit the User environment variable (e.g. some
# remoting/CI shells).
if (-not (Test-Path -Path $PROFILE)) {
    New-Item -ItemType File -Path $PROFILE -Force | Out-Null
}

$profileContent = ""
if (Test-Path -Path $PROFILE) {
    $profileContent = Get-Content -Path $PROFILE -Raw -ErrorAction SilentlyContinue
}

if (-not $profileContent -or ($profileContent -notlike "*$BinDir*")) {
    $exportLine = "if (`$env:Path.Split(';') -notcontains '$BinDir') { `$env:Path = '$BinDir;' + `$env:Path }"
    Add-Content -Path $PROFILE -Value "`n# Added by snovac install (make install)`n$exportLine"
    Write-Host "Updated PowerShell profile: $PROFILE"
}

Write-Host ""
Write-Host "snovac is now available in new PowerShell sessions."
Write-Host "To use it in this session right away, it has already been added to `$env:Path."
