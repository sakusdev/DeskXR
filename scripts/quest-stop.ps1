param(
    [switch]$KeepUnwornOverride
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "adb-common.ps1")
$adb = Resolve-DeskXRAdb

$devices = & $adb devices
if ($LASTEXITCODE -ne 0) {
    throw "adb devices failed."
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
    throw "More than one ADB device is connected. Disconnect the extra device first."
}

Write-Host "Stopping DeskXR on Quest..."
& $adb shell am force-stop org.sakus.deskxr
if ($LASTEXITCODE -ne 0) {
    throw "Failed to stop DeskXR on Quest."
}

if (-not $KeepUnwornOverride) {
    Write-Host "Restoring normal Quest proximity sensor behavior..."
    & $adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "DeskXR stopped, but the Quest proximity override could not be restored."
    }
}

Write-Host "DeskXR Quest bridge stopped."
if ($KeepUnwornOverride) {
    Write-Host "Quest unworn override was left enabled."
} else {
    Write-Host "Quest proximity sensor restored."
}
