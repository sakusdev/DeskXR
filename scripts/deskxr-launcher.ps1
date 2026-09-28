param(
    [string]$DriverPath = ""
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

if (-not $DriverPath) {
    if (Test-Path (Join-Path $PSScriptRoot "driver.vrdrivermanifest")) {
        $DriverPath = $PSScriptRoot
    } else {
        $DriverPath = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\build\deskxr"))
    }
}

function Get-HelperPath {
    param([string]$PackagedName, [string]$SourceName)

    $packaged = Join-Path $PSScriptRoot $PackagedName
    if (Test-Path $packaged) {
        return $packaged
    }

    $source = Join-Path $PSScriptRoot $SourceName
    if (Test-Path $source) {
        return $source
    }

    throw "Required DeskXR helper was not found: $PackagedName"
}

function Get-SettingsPath {
    $packaged = Join-Path $DriverPath "resources\settings\default.vrsettings"
    if (Test-Path $packaged) {
        return [System.IO.Path]::GetFullPath($packaged)
    }

    $source = Join-Path $PSScriptRoot "..\driver\resources\settings\default.vrsettings"
    if (Test-Path $source) {
        return [System.IO.Path]::GetFullPath($source)
    }

    return $null
}

function Invoke-Helper {
    param(
        [string]$ScriptPath,
        [string[]]$Arguments = @(),
        [switch]$Admin
    )

    $argumentList = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", ('"{0}"' -f $ScriptPath)
    ) + $Arguments

    $params = @{
        FilePath = "powershell.exe"
        ArgumentList = $argumentList
        Wait = $true
    }

    if ($Admin) {
        $params["Verb"] = "RunAs"
    }

    Start-Process @params
}

function Get-LanAddresses {
    try {
        return @(
            Get-NetIPConfiguration |
                Where-Object { $_.NetAdapter.Status -eq "Up" -and $_.IPv4Address } |
                ForEach-Object {
                    foreach ($address in $_.IPv4Address) {
                        if ($address.IPAddress -notlike "169.254.*" -and
                            $address.IPAddress -ne "127.0.0.1") {
                            [PSCustomObject]@{
                                Address = $address.IPAddress
                                Interface = $_.InterfaceAlias
                            }
                        }
                    }
                }
        )
    } catch {
        return @()
    }
}

function Read-DeskSettings {
    param([string]$Path)

    if (-not $Path -or -not (Test-Path $Path)) {
        return $null
    }

    try {
        return Get-Content -Raw -Path $Path | ConvertFrom-Json
    } catch {
        return $null
    }
}

function Save-DeskSettings {
    param(
        [string]$Path,
        [bool]$DesktopMono,
        [double]$HmdHeight,
        [double]$MouseSensitivity,
        [int]$WindowWidth,
        [int]$WindowHeight,
        [int]$RenderWidth,
        [int]$RenderHeight,
        [double]$DisplayFrequency
    )

    if (-not $Path) {
        throw "DeskXR default.vrsettings was not found."
    }

    $settings = Read-DeskSettings $Path
    if (-not $settings) {
        throw "Could not parse DeskXR settings: $Path"
    }

    $settings.driver_deskxr.hmd_y = $HmdHeight
    $settings.driver_deskxr.mouse_sensitivity_deg_per_pixel = $MouseSensitivity
    $settings.driver_deskxr.display_frequency_hz = $DisplayFrequency
    $settings.deskxr_display.window_width = $WindowWidth
    $settings.deskxr_display.window_height = $WindowHeight
    $settings.deskxr_display.render_width = $RenderWidth
    $settings.deskxr_display.render_height = $RenderHeight
    $settings.deskxr_display.desktop_mono = $DesktopMono

    $json = $settings | ConvertTo-Json -Depth 10
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8)
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "DeskXR"
$form.Size = New-Object System.Drawing.Size(660, 760)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.BackColor = [System.Drawing.Color]::FromArgb(24, 24, 24)
$form.ForeColor = [System.Drawing.Color]::White
$form.Font = New-Object System.Drawing.Font("Segoe UI", 10)

$title = New-Object System.Windows.Forms.Label
$title.Text = "DeskXR"
$title.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 24)
$title.Location = New-Object System.Drawing.Point(24, 18)
$title.Size = New-Object System.Drawing.Size(590, 45)
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = "Quest Touch -> SteamVR, with VRChat rendered on your monitor."
$subtitle.ForeColor = [System.Drawing.Color]::LightGray
$subtitle.Location = New-Object System.Drawing.Point(28, 66)
$subtitle.Size = New-Object System.Drawing.Size(590, 28)
$form.Controls.Add($subtitle)

$ipLabel = New-Object System.Windows.Forms.Label
$ipLabel.Text = "PC IPv4 address to enter on Quest"
$ipLabel.Location = New-Object System.Drawing.Point(28, 105)
$ipLabel.Size = New-Object System.Drawing.Size(300, 24)
$form.Controls.Add($ipLabel)

$ipBox = New-Object System.Windows.Forms.ComboBox
$ipBox.Location = New-Object System.Drawing.Point(28, 132)
$ipBox.Size = New-Object System.Drawing.Size(580, 30)
$ipBox.DropDownStyle = "DropDownList"
foreach ($entry in Get-LanAddresses) {
    [void]$ipBox.Items.Add(("{0}   ({1})" -f $entry.Address, $entry.Interface))
}
if ($ipBox.Items.Count -eq 0) {
    [void]$ipBox.Items.Add("No LAN IPv4 address detected")
}
$ipBox.SelectedIndex = 0
$form.Controls.Add($ipBox)

$status = New-Object System.Windows.Forms.Label
$status.Text = "Driver path: $DriverPath"
$status.ForeColor = [System.Drawing.Color]::Gray
$status.Location = New-Object System.Drawing.Point(28, 170)
$status.Size = New-Object System.Drawing.Size(580, 42)
$form.Controls.Add($status)

$settingsTitle = New-Object System.Windows.Forms.Label
$settingsTitle.Text = "VR performance / companion-window settings"
$settingsTitle.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 13)
$settingsTitle.Location = New-Object System.Drawing.Point(28, 218)
$settingsTitle.Size = New-Object System.Drawing.Size(250, 28)
$form.Controls.Add($settingsTitle)

$performanceLabel = New-Object System.Windows.Forms.Label
$performanceLabel.Text = "Virtual HMD render preset"
$performanceLabel.Location = New-Object System.Drawing.Point(28, 250)
$performanceLabel.Size = New-Object System.Drawing.Size(210, 24)
$form.Controls.Add($performanceLabel)

$performanceBox = New-Object System.Windows.Forms.ComboBox
$performanceBox.Location = New-Object System.Drawing.Point(240, 247)
$performanceBox.Size = New-Object System.Drawing.Size(330, 30)
$performanceBox.DropDownStyle = "DropDownList"
[void]$performanceBox.Items.Add("Performance — 960x960/eye @ 60 Hz")
[void]$performanceBox.Items.Add("Balanced — 1200x1200/eye @ 72 Hz")
[void]$performanceBox.Items.Add("Quality — 1600x1600/eye @ 90 Hz")
$performanceBox.SelectedIndex = 1
$form.Controls.Add($performanceBox)

$heightLabel = New-Object System.Windows.Forms.Label
$heightLabel.Text = "Virtual HMD height (m)"
$heightLabel.Location = New-Object System.Drawing.Point(28, 288)
$heightLabel.Size = New-Object System.Drawing.Size(180, 24)
$form.Controls.Add($heightLabel)

$heightBox = New-Object System.Windows.Forms.TextBox
$heightBox.Location = New-Object System.Drawing.Point(210, 286)
$heightBox.Size = New-Object System.Drawing.Size(90, 28)
$form.Controls.Add($heightBox)

$sensitivityLabel = New-Object System.Windows.Forms.Label
$sensitivityLabel.Text = "Mouse deg / pixel"
$sensitivityLabel.Location = New-Object System.Drawing.Point(330, 288)
$sensitivityLabel.Size = New-Object System.Drawing.Size(145, 24)
$form.Controls.Add($sensitivityLabel)

$sensitivityBox = New-Object System.Windows.Forms.TextBox
$sensitivityBox.Location = New-Object System.Drawing.Point(480, 286)
$sensitivityBox.Size = New-Object System.Drawing.Size(90, 28)
$form.Controls.Add($sensitivityBox)

$widthLabel = New-Object System.Windows.Forms.Label
$widthLabel.Text = "Desktop output size"
$widthLabel.Location = New-Object System.Drawing.Point(28, 330)
$widthLabel.Size = New-Object System.Drawing.Size(180, 24)
$form.Controls.Add($widthLabel)

$widthBox = New-Object System.Windows.Forms.TextBox
$widthBox.Location = New-Object System.Drawing.Point(210, 328)
$widthBox.Size = New-Object System.Drawing.Size(90, 28)
$form.Controls.Add($widthBox)

$xLabel = New-Object System.Windows.Forms.Label
$xLabel.Text = "x"
$xLabel.Location = New-Object System.Drawing.Point(310, 330)
$xLabel.Size = New-Object System.Drawing.Size(18, 24)
$form.Controls.Add($xLabel)

$heightPixelsBox = New-Object System.Windows.Forms.TextBox
$heightPixelsBox.Location = New-Object System.Drawing.Point(330, 328)
$heightPixelsBox.Size = New-Object System.Drawing.Size(90, 28)
$form.Controls.Add($heightPixelsBox)

function New-DeskButton {
    param(
        [string]$Text,
        [int]$X,
        [int]$Y,
        [int]$Width = 280
    )

    $button = New-Object System.Windows.Forms.Button
    $button.Text = $Text
    $button.Location = New-Object System.Drawing.Point($X, $Y)
    $button.Size = New-Object System.Drawing.Size($Width, 42)
    $button.FlatStyle = "System"
    $form.Controls.Add($button)
    return $button
}

$saveSettings = New-DeskButton "Save VR settings" 28 372
$useDisplay = New-DeskButton "Use primary monitor size" 328 372
$install = New-DeskButton "Install / update SteamVR driver" 28 430
$firewall = New-DeskButton "Install + allow UDP 39742" 328 430
$questStart = New-DeskButton "Start Quest bridge (ADB)" 28 486
$restore = New-DeskButton "Stop Quest bridge + restore" 328 486
$steamvr = New-DeskButton "Start SteamVR" 28 542
$installQuest = New-DeskButton "Install Quest APK..." 328 542

$mouseInfo = New-Object System.Windows.Forms.Label
$mouseInfo.Text = "Desktop head look: F8 = toggle mouse look, F9 = reset orientation"
$mouseInfo.ForeColor = [System.Drawing.Color]::LightGray
$mouseInfo.Location = New-Object System.Drawing.Point(28, 600)
$mouseInfo.Size = New-Object System.Drawing.Size(580, 28)
$form.Controls.Add($mouseInfo)

$calibrationInfo = New-Object System.Windows.Forms.Label
$calibrationInfo.Text = "Controller calibration: both triggers >80% + both stick clicks for ~1.2 s."
$calibrationInfo.ForeColor = [System.Drawing.Color]::LightGray
$calibrationInfo.Location = New-Object System.Drawing.Point(28, 630)
$calibrationInfo.Size = New-Object System.Drawing.Size(580, 28)
$form.Controls.Add($calibrationInfo)

$note = New-Object System.Windows.Forms.Label
$note.Text = "VRChat's desktop window adds GPU work on top of stereo VR. Use Performance/Balanced if GPU load is high."
$note.ForeColor = [System.Drawing.Color]::Gray
$note.Location = New-Object System.Drawing.Point(28, 666)
$note.Size = New-Object System.Drawing.Size(580, 44)
$form.Controls.Add($note)

$settingsPath = Get-SettingsPath
$current = Read-DeskSettings $settingsPath
if ($current) {
    $heightBox.Text = [Convert]::ToString(
        [double]$current.driver_deskxr.hmd_y,
        [System.Globalization.CultureInfo]::InvariantCulture)
    $sensitivityBox.Text = [Convert]::ToString(
        [double]$current.driver_deskxr.mouse_sensitivity_deg_per_pixel,
        [System.Globalization.CultureInfo]::InvariantCulture)
    $widthBox.Text = [string]$current.deskxr_display.window_width
    $heightPixelsBox.Text = [string]$current.deskxr_display.window_height

    $renderWidth = [int]$current.deskxr_display.render_width
    $frequency = [double]$current.driver_deskxr.display_frequency_hz

    if ($renderWidth -le 1000 -or $frequency -le 60.5) {
        $performanceBox.SelectedIndex = 0
    } elseif ($renderWidth -ge 1500 -or $frequency -ge 85.0) {
        $performanceBox.SelectedIndex = 2
    } else {
        $performanceBox.SelectedIndex = 1
    }
} else {
    $performanceBox.SelectedIndex = 1
    $heightBox.Text = "1.65"
    $sensitivityBox.Text = "0.08"
    $widthBox.Text = "1920"
    $heightPixelsBox.Text = "1080"
}

$saveSettings.Add_Click({
    try {
        $culture = [System.Globalization.CultureInfo]::InvariantCulture
        $hmdHeight = [Convert]::ToDouble($heightBox.Text, $culture)
        $mouseSensitivity = [Convert]::ToDouble($sensitivityBox.Text, $culture)
        $windowWidth = [Convert]::ToInt32($widthBox.Text, $culture)
        $windowHeight = [Convert]::ToInt32($heightPixelsBox.Text, $culture)

        if ($hmdHeight -lt 0.5 -or $hmdHeight -gt 3.0) {
            throw "Virtual HMD height must be between 0.5 and 3.0 meters."
        }
        if ($mouseSensitivity -le 0.0 -or $mouseSensitivity -gt 2.0) {
            throw "Mouse sensitivity must be greater than 0 and at most 2 deg/pixel."
        }
        if ($windowWidth -lt 320 -or $windowHeight -lt 240) {
            throw "Desktop output size is too small."
        }

        switch ($performanceBox.SelectedIndex) {
            0 {
                $renderWidth = 960
                $renderHeight = 960
                $displayFrequency = 60.0
            }
            2 {
                $renderWidth = 1600
                $renderHeight = 1600
                $displayFrequency = 90.0
            }
            default {
                $renderWidth = 1200
                $renderHeight = 1200
                $displayFrequency = 72.0
            }
        }

        Save-DeskSettings -Path $settingsPath -DesktopMono $false -HmdHeight $hmdHeight -MouseSensitivity $mouseSensitivity -WindowWidth $windowWidth -WindowHeight $windowHeight -RenderWidth $renderWidth -RenderHeight $renderHeight -DisplayFrequency $displayFrequency
        $status.Text = ("Saved: {0}x{1}/eye @ {2} Hz. Restart SteamVR." -f $renderWidth, $renderHeight, $displayFrequency)
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$useDisplay.Add_Click({
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $widthBox.Text = [string]$bounds.Width
    $heightPixelsBox.Text = [string]$bounds.Height
    $status.Text = "Primary monitor size copied. Press Save desktop settings."
})

$install.Add_Click({
    try {
        $script = Get-HelperPath "install.ps1" "install-driver.ps1"
        Invoke-Helper $script @("-DriverPath", ('"{0}"' -f $DriverPath))
        $status.Text = "Driver install/update command completed. Restart SteamVR."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$firewall.Add_Click({
    try {
        $script = Get-HelperPath "install.ps1" "install-driver.ps1"
        Invoke-Helper $script @("-DriverPath", ('"{0}"' -f $DriverPath), "-ConfigureFirewall") -Admin
        $status.Text = "Driver + firewall setup completed. Restart SteamVR."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$questStart.Add_Click({
    try {
        $selected = [string]$ipBox.SelectedItem
        $hostAddress = ($selected -split "\s+")[0]

        try {
            [void][System.Net.IPAddress]::Parse($hostAddress)
        } catch {
            throw "Select a valid LAN IPv4 address first."
        }

        $script = Get-HelperPath "quest-start.ps1" "quest-start.ps1"
        Invoke-Helper $script @("-HostAddress", $hostAddress, "-Port", "39742")
        $status.Text = "Quest DeskXR bridge launch requested via ADB."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$restore.Add_Click({
    try {
        $script = Get-HelperPath "quest-stop.ps1" "quest-stop.ps1"
        Invoke-Helper $script
        $status.Text = "Quest DeskXR bridge stopped and proximity sensor restored."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$steamvr.Add_Click({
    Start-Process "steam://rungameid/250820"
})

$installQuest.Add_Click({
    try {
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = "Select DeskXR Quest APK"
        $dialog.Filter = "Android APK (*.apk)|*.apk|All files (*.*)|*.*"
        $dialog.CheckFileExists = $true

        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            return
        }

        $script = Get-HelperPath "quest-install.ps1" "quest-install.ps1"
        Invoke-Helper $script @("-ApkPath", ('"{0}"' -f $dialog.FileName))
        $status.Text = "Quest APK install command completed."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

[void]$form.ShowDialog()
