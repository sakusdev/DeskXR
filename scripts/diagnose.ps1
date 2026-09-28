param(
    [int]$UdpPort = 39742
)

$ErrorActionPreference = "Continue"

Write-Host "=== DeskXR diagnostics ==="
Write-Host ""

Write-Host "[ADB]"
. (Join-Path $PSScriptRoot "adb-common.ps1")
$adb = Resolve-DeskXRAdb -AllowMissing
if (-not $adb) {
    Write-Host "adb: NOT FOUND"
    Write-Host ("Portable location: {0}" -f (Join-Path $PSScriptRoot "platform-tools\adb.exe"))
} else {
    Write-Host ("adb: {0}" -f $adb)
    & $adb devices
    Write-Host ""

    Write-Host "Quest package:"
    & $adb shell pm path org.sakus.deskxr 2>$null

    Write-Host ""
    Write-Host "Quest foreground/activity hint:"
    & $adb shell dumpsys activity activities 2>$null |
        Select-String "org.sakus.deskxr|mResumedActivity" |
        Select-Object -First 12

    Write-Host ""
    Write-Host "Recent DeskXR Quest logs:"
    & $adb logcat -d -s "DeskXR:I" "*:S" 2>$null |
        Select-Object -Last 30
}

Write-Host ""
Write-Host "[Network]"
try {
    Get-NetIPConfiguration |
        Where-Object { $_.NetAdapter.Status -eq "Up" -and $_.IPv4Address } |
        ForEach-Object {
            foreach ($address in $_.IPv4Address) {
                if ($address.IPAddress -notlike "169.254.*") {
                    Write-Host ("{0,-16} {1}" -f $address.IPAddress, $_.InterfaceAlias)
                }
            }
        }
} catch {
    Write-Host "Could not enumerate IP configuration."
}

try {
    $firewall = Get-NetFirewallRule -DisplayName "DeskXR UDP $UdpPort" -ErrorAction SilentlyContinue
    if ($firewall) {
        Write-Host ("Firewall rule: present ({0})" -f $firewall.Enabled)
    } else {
        Write-Host "Firewall rule: NOT FOUND"
    }
} catch {
    Write-Host "Firewall rule: could not query"
}

Write-Host ""
Write-Host "[SteamVR]"
$steamRoots = @(
    (Join-Path ([Environment]::GetFolderPath("ProgramFilesX86")) "Steam")
)

try {
    $regSteam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction Stop).SteamPath
    if ($regSteam) {
        $steamRoots += $regSteam
    }
} catch {
}

$steamRoots = $steamRoots | Select-Object -Unique
$foundLog = $false

foreach ($root in $steamRoots) {
    $log = Join-Path $root "logs\vrserver.txt"
    if (Test-Path $log) {
        $foundLog = $true
        Write-Host ("vrserver log: {0}" -f $log)
        Get-Content $log -Tail 400 |
            Select-String "DeskXR|deskxr" |
            Select-Object -Last 30
        break
    }
}

if (-not $foundLog) {
    Write-Host "vrserver.txt: not found in detected Steam roots"
}

Write-Host ""
Write-Host "If the Quest log reaches 'OpenXR focused' and reports packets/s while SteamVR logs"
Write-Host "show the DeskXR HMD/controllers activated, the bridge itself is alive."
