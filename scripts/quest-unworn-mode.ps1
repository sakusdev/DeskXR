param(
    [switch]$Disable
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "adb-common.ps1")
$adb = Resolve-DeskXRAdb

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    & $adb @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed with exit code $LASTEXITCODE"
    }
}

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

if ($Disable) {
    Write-Host "Returning Quest proximity handling to the physical sensor..."
    Invoke-Adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable
    Write-Host "Quest unworn override disabled."
    exit
}

Write-Host "Forcing Quest proximity state to 'worn' for DeskXR testing..."
Invoke-Adb shell am broadcast -a com.oculus.vrpowermanager.prox_close

Write-Host ""
Write-Host "Quest should now stay active when removed from your face."
Write-Host "Run this when finished to restore normal proximity behavior:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts/quest-unworn-mode.ps1 -Disable"
