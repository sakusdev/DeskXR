param(
    [Parameter(Mandatory = $true)]
    [string]$ApkPath
)

$ErrorActionPreference = "Stop"

$adb = Get-Command adb -ErrorAction SilentlyContinue
if (-not $adb) {
    throw "adb was not found in PATH. Install Android platform-tools first."
}

$resolvedApk = Resolve-Path -Path $ApkPath -ErrorAction Stop
if ([System.IO.Path]::GetExtension($resolvedApk.Path) -ne ".apk") {
    throw "The selected file is not an APK."
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
    throw "More than one ADB device is connected. Disconnect the extra device first."
}

Write-Host "Installing DeskXR APK..."
$installOutput = & $adb.Source install -r $resolvedApk.Path 2>&1

if ($LASTEXITCODE -ne 0 -or
    ($installOutput -join [Environment]::NewLine) -match "Failure \[") {
    throw ("APK install failed. " + ($installOutput -join " "))
}

$installOutput | Write-Host
Write-Host "DeskXR APK installed."
