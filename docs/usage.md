# DeskXR quick start

This guide assumes a Quest 3 or Quest 3S, Windows 10/11, SteamVR, and an ADB-authorized Quest.

## 1. Install the Quest APK

Install the `DeskXR-Quest-debug` artifact APK with the Windows launcher or ADB:

~~~powershell
adb install -r .\quest-debug.apk
~~~

Recent builds open a normal 2D **DeskXR Setup** activity first when launched from the Quest app library. Older builds marked the setup activity as immersive and can appear stuck on Horizon's OVR/OpenXR session startup screen. For those builds, use the PC-driven ADB launch described below.

## 2. Prepare the Windows driver

Download and extract the `DeskXR-Windows-x64` artifact to a permanent location such as:

~~~text
C:\DeskXR
~~~

Do not move/delete the extracted folder after registering the driver: SteamVR stores the driver path.

The folder should contain at least:

~~~text
driver.vrdrivermanifest
bin\win64\driver_deskxr.dll
resources\
install.ps1
DeskXR.ps1
quest-start.ps1
quest-stop.ps1
~~~

Open PowerShell in that folder and launch:

~~~powershell
powershell -ExecutionPolicy Bypass -File .\DeskXR.ps1
~~~

In the DeskXR window:

1. select the LAN IPv4 address that the Quest can reach;
2. optionally press **Use primary monitor size**, then **Save desktop settings**;
3. press **Install + allow UDP 39742** and approve the Administrator prompt;
4. fully exit SteamVR if it is already running;
5. press **Start SteamVR**.

The driver should expose one virtual HMD and two virtual controllers.

## 3. Start the Quest bridge

Keep the Quest connected through authorized ADB for the first test.

You do **not** need to add ADB to the Windows environment `PATH`. For a portable setup, extract Android platform-tools so this path exists next to the DeskXR scripts:

~~~text
DeskXR-Windows-x64\platform-tools\adb.exe
~~~

All packaged Quest helpers and `diagnose.ps1` automatically find that local copy. They also fall back to the Android SDK environment variables/default SDK location and then `PATH`.

If you are testing ADB manually from PowerShell without a global PATH entry, use:

~~~powershell
.\platform-tools\adb.exe devices
~~~

The Quest should appear with status `device`, not `unauthorized`.

In the DeskXR Windows launcher press:

**Start Quest bridge (ADB)**

That helper:

- applies Meta's development proximity override so the Quest can stay awake off your face;
- passes the selected PC IPv4 address and UDP port 39742 to the Quest;
- launches the immersive OpenXR activity;
- asks DeskXR to start streaming automatically.

Equivalent command:

~~~powershell
powershell -ExecutionPolicy Bypass -File .\quest-start.ps1 -HostAddress 192.168.1.20
~~~

Replace the example address with your PC address.

## 4. Check that the bridge is alive

Quest native logs:

~~~powershell
adb logcat -s DeskXR:I '*:S'
~~~

A healthy bridge should progress through messages similar to:

~~~text
Starting OpenXR bridge...
OpenXR initialized. Waiting for session...
Streaming to 192.168.x.x:39742 | OpenXR focused | ... packets/s | PC linked | RTT ...
~~~

The Quest display is not the VR output. DeskXR intentionally renders the VR application on the PC monitor.

If the headset still shows an OS/OpenXR startup screen but the log says `OpenXR focused`, packets are flowing, and the PC says linked, the Quest-side picture is not important for DeskXR operation.

## 5. Verify the SteamVR driver

SteamVR's `vrserver.txt` should contain DeskXR messages such as:

~~~text
[DeskXR] listening on UDP 39742
[DeskXR] virtual HMD activated
[DeskXR] left controller activated
[DeskXR] right controller activated
~~~

A common Steam install log can be inspected with:

~~~powershell
Get-Content "$env:ProgramFiles(x86)\Steam\logs\vrserver.txt" -Tail 300 |
    Select-String DeskXR
~~~

If SteamVR previously crashed while loading a driver, also check:

**SteamVR > Settings > Startup/Shutdown > Manage Add-ons**

and make sure DeskXR is enabled.

## 6. Launch VRChat

With SteamVR running and the Quest bridge streaming, launch VRChat in VR mode.

DeskXR supplies:

- virtual HMD;
- left/right 6DoF controller poses;
- controller buttons/sticks/touch;
- haptics.

OSC FBT can continue to be sent directly to VRChat separately.

## 7. Calibrate controller space

Put the Quest on/above the monitor with cameras facing you.

Hold both controllers shoulder-width apart in front of the upper chest, then either:

- use **Quick calibrate** in the Quest setup/bridge UI; or
- hold both triggers above ~80% and click both thumbsticks for about 1.2 seconds.

Both controllers vibrate when controller-only calibration succeeds.

## 8. Desktop head control

- **F8**: toggle mouse-look capture.
- **F9**: reset virtual head yaw/pitch.

## 9. Shut down

Use **Stop Quest bridge + restore** in the Windows launcher.

This force-stops DeskXR on Quest and restores normal proximity sensor behavior.

## Troubleshooting the OVR/OpenXR startup screen

### Old APK: setup screen never appears

This was a DeskXR packaging bug: the same Android activity was both the ordinary app launcher and the immersive OpenXR activity. Horizon could enter the VR session startup path before the IP/start UI was usable.

Use **Start Quest bridge (ADB)** as a workaround, or install a newer APK that contains `SetupActivity`.

### Log reaches `OpenXR focused`

The OpenXR session is active. The visible Quest loading/black screen is not itself a blocker because the Quest display is intentionally unused.

### Log stops at `OpenXR initialized. Waiting for session...`

The runtime has not transitioned the session into a runnable state. Capture:

~~~powershell
adb logcat -d -s DeskXR:I '*:S'
~~~

and the latest full Android log around the launch for debugging.

### Quest says `PC no-ack`

Check:

- correct PC LAN IPv4 address;
- both devices are on the same reachable network;
- Windows network profile/firewall permits UDP 39742;
- SteamVR is running with the DeskXR driver loaded.

### SteamVR does not see DeskXR

Re-run **Install + allow UDP 39742**, restart SteamVR completely, and check `vrserver.txt` plus Manage Add-ons.
