param(
    [string]$BuildDir = "build-win",
    [string]$DeploySubdir = "install/frp-deploy"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir
$DeployDir = Join-Path $ProjectDir $BuildDir | Join-Path -ChildPath $DeploySubdir

Write-Host "==> FRP Deploy Package (Windows supplement)"
Write-Host "    Build dir:   $BuildDir"
Write-Host "    Output dir:  $BuildDir/$DeploySubdir"

if (-not (Test-Path $DeployDir)) {
    Write-Host "ERROR: Deploy directory not found at $BuildDir/$DeploySubdir"
    Write-Host "       Copy the Linux deploy package there first."
    exit 1
}

$WinBuildDir = Join-Path $ProjectDir "build-win"
if (-not (Test-Path $WinBuildDir)) {
    Write-Host "ERROR: Windows build directory not found: build-win"
    exit 1
}

# Copy Windows binaries and DLLs
Write-Host "==> Copying Windows binaries..."

$Binaries = @("frp_proxy_server", "frp_proxy_client", "frp_proxy_accessor")
foreach ($bin in $Binaries) {
    $src = Join-Path $WinBuildDir "applications/$bin/Release/$bin.exe"
    if (-not (Test-Path $src)) {
        Write-Host "ERROR: $src not found. Please build Windows first."
        exit 1
    }
    Copy-Item $src $DeployDir
    Write-Host "    $bin.exe"
}

Write-Host "==> Copying Windows runtime DLLs..."
$DllSrc = Join-Path $WinBuildDir "applications/frp_proxy_server/Release"
Copy-Item (Join-Path $DllSrc "libcrypto-3-x64.dll") $DeployDir
Copy-Item (Join-Path $DllSrc "libssl-3-x64.dll") $DeployDir
Write-Host "    libcrypto-3-x64.dll  libssl-3-x64.dll"

# Generate Windows launch scripts
Write-Host "==> Generating Windows launch scripts..."

@'
@echo off
pushd %~dp0
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=server.json
if exist "%CONFIG%.overlay" set CONFIG=%CONFIG%.overlay
frp_proxy_server.exe --config "%CONFIG%"
'@ | Out-File -FilePath (Join-Path $DeployDir "launch_server.bat") -Encoding ASCII

@'
@echo off
pushd %~dp0
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=provider.json
if exist "%CONFIG%.overlay" set CONFIG=%CONFIG%.overlay
frp_proxy_client.exe --config "%CONFIG%"
'@ | Out-File -FilePath (Join-Path $DeployDir "launch_provider.bat") -Encoding ASCII

@'
@echo off
pushd %~dp0
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=accessor.json
if exist "%CONFIG%.overlay" set CONFIG=%CONFIG%.overlay
frp_proxy_accessor.exe --config "%CONFIG%"
'@ | Out-File -FilePath (Join-Path $DeployDir "launch_accessor.bat") -Encoding ASCII

Write-Host "    launch_server.bat  launch_provider.bat  launch_accessor.bat"

# Summary
Write-Host ""
Write-Host "==> Windows files added to: $DeployDir"
Write-Host ""
Get-ChildItem $DeployDir -Filter *.exe | Format-Table Name, Length
Get-ChildItem $DeployDir -Filter *.dll | Format-Table Name, Length
Get-ChildItem $DeployDir -Filter *.bat | Format-Table Name, Length
