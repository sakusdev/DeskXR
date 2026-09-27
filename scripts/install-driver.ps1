param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$driverRoot = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "deskxr"))
$programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
$vrPathReg = Join-Path $programFilesX86 "Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"

if (-not (Test-Path $vrPathReg)) {
    throw "vrpathreg.exe was not found at: $vrPathReg"
}

if ($Remove) {
    & $vrPathReg removedriver $driverRoot
    Write-Host "DeskXR driver removed: $driverRoot"
    exit
}

if (-not (Test-Path (Join-Path $driverRoot "driver.vrdrivermanifest"))) {
    throw "DeskXR driver package was not found at: $driverRoot. Build the project first."
}

& $vrPathReg adddriver $driverRoot
Write-Host "DeskXR driver registered: $driverRoot"
Write-Host "Restart SteamVR."
