param(
    [string]$DriverPath = "",
    [int]$UdpPort = 39742,
    [switch]$Remove,
    [switch]$ConfigureFirewall
)

$ErrorActionPreference = "Stop"

function Get-SteamRoots {
    $roots = New-Object System.Collections.Generic.List[string]

    $registryCandidates = @(
        @{ Path = "HKCU:\Software\Valve\Steam"; Name = "SteamPath" },
        @{ Path = "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam"; Name = "InstallPath" },
        @{ Path = "HKLM:\SOFTWARE\Valve\Steam"; Name = "InstallPath" }
    )

    foreach ($candidate in $registryCandidates) {
        try {
            $value = (Get-ItemProperty -Path $candidate.Path -Name $candidate.Name -ErrorAction Stop).($candidate.Name)
            if ($value -and -not $roots.Contains($value)) {
                $roots.Add($value)
            }
        } catch {
        }
    }

    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    if ($programFilesX86) {
        $defaultSteam = Join-Path $programFilesX86 "Steam"
        if ((Test-Path $defaultSteam) -and -not $roots.Contains($defaultSteam)) {
            $roots.Add($defaultSteam)
        }
    }

    return $roots
}

function Get-SteamLibraryRoots {
    $libraries = New-Object System.Collections.Generic.List[string]

    foreach ($steamRoot in Get-SteamRoots) {
        if (-not $libraries.Contains($steamRoot)) {
            $libraries.Add($steamRoot)
        }

        $libraryFile = Join-Path $steamRoot "steamapps\libraryfolders.vdf"
        if (-not (Test-Path $libraryFile)) {
            continue
        }

        foreach ($line in Get-Content $libraryFile -ErrorAction SilentlyContinue) {
            if ($line -match '"path"\s+"([^"]+)"') {
                $path = $Matches[1] -replace '\\\\', '\'
                if ((Test-Path $path) -and -not $libraries.Contains($path)) {
                    $libraries.Add($path)
                }
            }
        }
    }

    return $libraries
}

function Find-VrPathReg {
    foreach ($library in Get-SteamLibraryRoots) {
        $candidate = Join-Path $library "steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
        if (Test-Path $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    throw "SteamVR vrpathreg.exe was not found. Install SteamVR first."
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $DriverPath) {
    $artifactManifest = Join-Path $PSScriptRoot "driver.vrdrivermanifest"
    if (Test-Path $artifactManifest) {
        $DriverPath = $PSScriptRoot
    } else {
        $DriverPath = Join-Path $PSScriptRoot "..\build\deskxr"
    }
}

$driverRoot = [System.IO.Path]::GetFullPath($DriverPath)
$manifest = Join-Path $driverRoot "driver.vrdrivermanifest"
$vrPathReg = Find-VrPathReg
$firewallName = "DeskXR UDP $UdpPort"

if ($Remove) {
    & $vrPathReg removedriver $driverRoot

    if (Test-IsAdministrator) {
        Get-NetFirewallRule -DisplayName $firewallName -ErrorAction SilentlyContinue |
            Remove-NetFirewallRule -ErrorAction SilentlyContinue
    }

    Write-Host "DeskXR driver removed: $driverRoot"
    exit
}

if (-not (Test-Path $manifest)) {
    throw "DeskXR driver package was not found at: $driverRoot"
}

& $vrPathReg adddriver $driverRoot
if ($LASTEXITCODE -ne 0) {
    throw "vrpathreg failed with exit code $LASTEXITCODE"
}

if ($ConfigureFirewall) {
    if (Test-IsAdministrator) {
        Get-NetFirewallRule -DisplayName $firewallName -ErrorAction SilentlyContinue |
            Remove-NetFirewallRule -ErrorAction SilentlyContinue

        New-NetFirewallRule -DisplayName $firewallName -Direction Inbound -Action Allow -Protocol UDP -LocalPort $UdpPort -Profile Private | Out-Null
        Write-Host "Windows Firewall: UDP $UdpPort allowed on Private networks."
    } else {
        Write-Warning "Firewall configuration requested, but PowerShell is not running as Administrator."
        Write-Warning "Re-run this script as Administrator with -ConfigureFirewall."
    }
}

Write-Host ""
Write-Host "DeskXR driver registered: $driverRoot"
Write-Host "SteamVR registry tool: $vrPathReg"
Write-Host ""
Write-Host "Possible PC IPv4 addresses for the Quest app:"

try {
    Get-NetIPConfiguration |
        Where-Object { $_.NetAdapter.Status -eq "Up" -and $_.IPv4Address } |
        ForEach-Object {
            foreach ($address in $_.IPv4Address) {
                if ($address.IPAddress -notlike "169.254.*") {
                    Write-Host ("  {0,-16} {1}" -f $address.IPAddress, $_.InterfaceAlias)
                }
            }
        }
} catch {
    Write-Warning "Could not enumerate local IPv4 addresses."
}

Write-Host ""
Write-Host "Restart SteamVR after installing or updating the driver."
Write-Host "Quest should send to UDP port $UdpPort."
