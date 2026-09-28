param(
    [Parameter(Mandatory = $true)]
    [string]$HostAddress,

    [int]$Port = 39742,

    [switch]$SkipUnwornOverride
)

$ErrorActionPreference = "Stop"

$adb = Get-Command adb -ErrorAction SilentlyContinue
if (-not $adb) {
    throw "adb was not found in PATH. Install Android platform-tools first."
}

try {
    $parsedAddress = [System.Net.IPAddress]::Parse($HostAddress)
    if ($parsedAddress.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
        throw "IPv6 is not supported by DeskXR v1."
    }
} catch {
    throw "HostAddress must be a valid IPv4 address."
}

if ($Port -lt 1 -or $Port -gt 65535) {
    throw "Port must be between 1 and 65535."
}

$devices = & $adb.Source devices
if ($LASTEXITCODE -ne 0) {
    throw "adb devices failed."
}

$packagePath = & $adb.Source shell pm path org.sakus.deskxr 2>$null
if ($LASTEXITCODE -ne 0 -or -not ($packagePath -match "^package:")) {
    throw "DeskXR is not installed on the connected Quest. Install the latest DeskXR-Quest APK first."
}

$activityDump = & $adb.Source shell dumpsys package org.sakus.deskxr 2>$null
$hasMainActivity = ($activityDump -join [Environment]::NewLine) -match "org\.sakus\.deskxr\.MainActivity"

if (-not $hasMainActivity) {
    throw "The installed DeskXR APK is an old/incompatible build: MainActivity is missing. Uninstall org.sakus.deskxr and install the latest DeskXR-Quest APK from the current PR/Actions build."
}

$connected = @(
    $devices |
        Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" }
)

if ($connected.Count -eq 0) {
    throw "No authorized Quest device is connected over ADB."
}

if ($connected.Count -gt 1) {
    throw "More than one ADB device is connected. Disconnect the extra device or launch DeskXR manually."
}

if (-not $SkipUnwornOverride) {
    Write-Host "Keeping Quest awake while unworn..."
    & $adb.Source shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to enable Quest unworn mode."
    }
}

Write-Host ("Launching DeskXR Quest bridge -> {0}:{1}" -f $HostAddress, $Port)

$launchOutput = & $adb.Source shell am start -S -n org.sakus.deskxr/.MainActivity --es deskxr_host $HostAddress --ei deskxr_port $Port --ez deskxr_autostart true 2>&1

if ($LASTEXITCODE -ne 0 -or
    ($launchOutput -join [Environment]::NewLine) -match "Error type|does not exist|Unable to resolve") {
    throw ("Failed to launch DeskXR on Quest. " + ($launchOutput -join " "))
}

$launchOutput | Write-Host

Write-Host ""
Write-Host "DeskXR was launched on Quest with autostart enabled."
Write-Host "Use quest-unworn-mode.ps1 -Disable when finished to restore the proximity sensor."
