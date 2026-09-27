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
    [void][System.Net.IPAddress]::Parse($HostAddress)
} catch {
    throw "HostAddress must be a valid IP address."
}

if ($Port -lt 1 -or $Port -gt 65535) {
    throw "Port must be between 1 and 65535."
}

$devices = & $adb.Source devices
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

& $adb.Source shell am start -S -n org.sakus.deskxr/.MainActivity --es deskxr_host $HostAddress --ei deskxr_port $Port --ez deskxr_autostart true

if ($LASTEXITCODE -ne 0) {
    throw "Failed to launch DeskXR on Quest."
}

Write-Host ""
Write-Host "DeskXR was launched on Quest with autostart enabled."
Write-Host "Use quest-unworn-mode.ps1 -Disable when finished to restore the proximity sensor."
