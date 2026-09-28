# Shared ADB discovery for DeskXR PowerShell helpers.
#
# Supported layouts, in priority order:
#   DeskXR\adb.exe
#   DeskXR\platform-tools\adb.exe
#   DeskXR\tools\platform-tools\adb.exe
#
# When running from the source tree, the same layouts are also checked one
# directory above scripts\. Android SDK environment variables and PATH are
# used as fallbacks.

$script:DeskXRAdbCommonRoot = $PSScriptRoot

function Resolve-DeskXRAdb {
    [CmdletBinding()]
    param(
        [switch]$AllowMissing
    )

    $root = $script:DeskXRAdbCommonRoot

    $candidates = New-Object System.Collections.Generic.List[string]

    foreach ($relative in @(
        "adb.exe",
        "platform-tools\adb.exe",
        "tools\platform-tools\adb.exe",
        "android-platform-tools\adb.exe",
        "..\adb.exe",
        "..\platform-tools\adb.exe",
        "..\tools\platform-tools\adb.exe"
    )) {
        $candidate = Join-Path $root $relative
        if (-not $candidates.Contains($candidate)) {
            $candidates.Add($candidate)
        }
    }

    foreach ($sdkRoot in @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if ($sdkRoot) {
            $candidate = Join-Path $sdkRoot "platform-tools\adb.exe"
            if (-not $candidates.Contains($candidate)) {
                $candidates.Add($candidate)
            }
        }
    }

    if ($env:LOCALAPPDATA) {
        $candidate = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"
        if (-not $candidates.Contains($candidate)) {
            $candidates.Add($candidate)
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    $pathAdb = Get-Command adb.exe -ErrorAction SilentlyContinue
    if (-not $pathAdb) {
        $pathAdb = Get-Command adb -ErrorAction SilentlyContinue
    }

    if ($pathAdb) {
        return $pathAdb.Source
    }

    if ($AllowMissing) {
        return $null
    }

    $expected = Join-Path $root "platform-tools\adb.exe"
    throw @"
adb.exe was not found.

For a portable DeskXR install, extract Google's Android platform-tools folder here:

  $root\platform-tools\

so that this file exists:

  $expected

DeskXR also checks ANDROID_SDK_ROOT, ANDROID_HOME, the default Android SDK folder, and PATH.
"@
}
