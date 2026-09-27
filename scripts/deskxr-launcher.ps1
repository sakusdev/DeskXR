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

$form = New-Object System.Windows.Forms.Form
$form.Text = "DeskXR"
$form.Size = New-Object System.Drawing.Size(620, 560)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.BackColor = [System.Drawing.Color]::FromArgb(24, 24, 24)
$form.ForeColor = [System.Drawing.Color]::White
$form.Font = New-Object System.Drawing.Font("Segoe UI", 10)

$title = New-Object System.Windows.Forms.Label
$title.Text = "DeskXR"
$title.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 24)
$title.Location = New-Object System.Drawing.Point(24, 20)
$title.Size = New-Object System.Drawing.Size(550, 45)
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = "Quest Touch -> SteamVR, with VRChat rendered on your monitor."
$subtitle.ForeColor = [System.Drawing.Color]::LightGray
$subtitle.Location = New-Object System.Drawing.Point(28, 70)
$subtitle.Size = New-Object System.Drawing.Size(550, 28)
$form.Controls.Add($subtitle)

$ipLabel = New-Object System.Windows.Forms.Label
$ipLabel.Text = "PC IPv4 address to enter on Quest"
$ipLabel.Location = New-Object System.Drawing.Point(28, 112)
$ipLabel.Size = New-Object System.Drawing.Size(300, 24)
$form.Controls.Add($ipLabel)

$ipBox = New-Object System.Windows.Forms.ComboBox
$ipBox.Location = New-Object System.Drawing.Point(28, 140)
$ipBox.Size = New-Object System.Drawing.Size(540, 30)
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
$status.Location = New-Object System.Drawing.Point(28, 180)
$status.Size = New-Object System.Drawing.Size(540, 48)
$form.Controls.Add($status)

function New-DeskButton {
    param(
        [string]$Text,
        [int]$X,
        [int]$Y,
        [int]$Width = 260
    )

    $button = New-Object System.Windows.Forms.Button
    $button.Text = $Text
    $button.Location = New-Object System.Drawing.Point($X, $Y)
    $button.Size = New-Object System.Drawing.Size($Width, 42)
    $button.FlatStyle = "System"
    $form.Controls.Add($button)
    return $button
}

$install = New-DeskButton "Install / update SteamVR driver" 28 240
$firewall = New-DeskButton "Install + allow UDP 39742" 308 240
$unworn = New-DeskButton "Keep Quest active while unworn" 28 296
$restore = New-DeskButton "Restore Quest proximity sensor" 308 296
$steamvr = New-DeskButton "Start SteamVR" 28 352
$openPr = New-DeskButton "Open DeskXR GitHub" 308 352

$mouseInfo = New-Object System.Windows.Forms.Label
$mouseInfo.Text = "Desktop head look: F8 = toggle mouse look, F9 = reset orientation"
$mouseInfo.ForeColor = [System.Drawing.Color]::LightGray
$mouseInfo.Location = New-Object System.Drawing.Point(28, 416)
$mouseInfo.Size = New-Object System.Drawing.Size(540, 28)
$form.Controls.Add($mouseInfo)

$note = New-Object System.Windows.Forms.Label
$note.Text = "The unworn mode uses Meta's documented ADB proximity override. Restore the sensor when you are done testing."
$note.ForeColor = [System.Drawing.Color]::Gray
$note.Location = New-Object System.Drawing.Point(28, 452)
$note.Size = New-Object System.Drawing.Size(540, 50)
$form.Controls.Add($note)

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

$unworn.Add_Click({
    try {
        $script = Get-HelperPath "quest-unworn-mode.ps1" "quest-unworn-mode.ps1"
        Invoke-Helper $script
        $status.Text = "Quest proximity override requested."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$restore.Add_Click({
    try {
        $script = Get-HelperPath "quest-unworn-mode.ps1" "quest-unworn-mode.ps1"
        Invoke-Helper $script @("-Disable")
        $status.Text = "Quest proximity sensor restored."
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "DeskXR") | Out-Null
    }
})

$steamvr.Add_Click({
    Start-Process "steam://rungameid/250820"
})

$openPr.Add_Click({
    Start-Process "https://github.com/sakusdev/DeskXR"
})

[void]$form.ShowDialog()
