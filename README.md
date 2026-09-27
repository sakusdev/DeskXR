# DeskXR

DeskXR is an experimental **HMD-less desktop VR bridge**.

The goal is to keep VRChat in VR mode while you look at a normal PC monitor, use Quest Touch controllers as real 6DoF hands, and optionally let VRChat receive OSC full-body trackers at the same time.

## Current end-to-end PoC

The repository now contains both halves of the first working pipeline:

- **Windows / SteamVR driver**
  - virtual HMD with fixed position and optional mouse-driven yaw/pitch;
  - monitor-oriented compositor output with a full-size monocular desktop mode or stereo side-by-side mode;
  - left/right virtual controllers;
  - UDP input for 6DoF pose and controller actions;
  - trigger, grip, thumbstick, A/B/X/Y, menu;
  - stale-packet tracking loss;
  - finite-difference linear/angular velocity estimation for SteamVR pose prediction;
  - SteamVR haptic events are forwarded back to the matching Quest Touch controller;
  - reverse-link acknowledgements let the Quest app detect whether the PC driver is actually receiving packets.
- **Quest 3 / Quest 3S client**
  - native Android + OpenXR;
  - Oculus Touch interaction profile;
  - left/right grip poses;
  - controller inputs;
  - low-work frame loop with no PC video streaming;
  - UDP streaming directly to the PC driver;
  - simple in-headset UI for the PC IPv4 address and port.
- **CI**
  - Windows driver artifact;
  - arm64 Quest debug APK artifact.

Both Windows and Quest builds currently pass in GitHub Actions.

## Architecture

    Quest Touch L/R
          |
       OpenXR
          |
          v
    DeskXR Quest APK
          |
      UDP :39742
          |
          v
    DeskXR SteamVR driver
       /             \
 virtual HMD      virtual hands
       \             /
            SteamVR
               |
             VRChat
               |
          PC monitor

OSC FBT intentionally stays separate. If VRChat is running in VR mode through DeskXR, OSC trackers can still be supplied directly to VRChat.

## Build the Windows driver

Requirements: Windows 10/11 x64, Visual Studio 2022 C++ tools, CMake 3.20+, and SteamVR.

~~~powershell
cmake -S . -B build -A x64
cmake --build build --config Release
~~~

The packaged driver is written to:

    build/deskxr

It is self-installing: the package includes `install.ps1`. From a built tree you can also use the source script directly:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts/install-driver.ps1
~~~

To also create the Windows Firewall rule for UDP 39742, run an elevated PowerShell:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts/install-driver.ps1 -ConfigureFirewall
~~~

The installer searches Steam libraries for SteamVR, registers the driver with `vrpathreg.exe`, and prints likely LAN IPv4 addresses to enter in the Quest app. Restart SteamVR after installing.

## Desktop output

`deskxr_display.desktop_mono` defaults to `true`. In that mode both VR eyes are mapped onto the same full desktop viewport so the monitor gets a normal full-size monocular view instead of a horizontally squeezed stereo image. Set it to `false` for conventional side-by-side output.

The launcher can edit this option and the desktop output resolution. These settings are read when SteamVR loads the driver, so restart SteamVR after changing them.

## Desktop head look

DeskXR keeps the virtual HMD position fixed, but its orientation can now be controlled from the desktop:

- **F8** toggles mouse-look capture;
- **F9** resets virtual head yaw/pitch;
- while mouse look is active, the cursor is re-centered and mouse movement rotates the virtual HMD;
- sensitivity and pitch limit are configurable in `driver/resources/settings/default.vrsettings`.

This is optional. If you leave mouse look off, VRChat can still use the controller sticks for locomotion/turning.

## PC-only smoke test

Before involving Quest, test the SteamVR half:

~~~powershell
python tools/sim_sender.py
~~~

The simulator sends two moving hand poses to UDP port 39742. SteamVR should see one virtual HMD plus left/right controllers.

## Build the Quest client

Open the project in Android Studio, or use Gradle 8.10.x with Android SDK 35, NDK 27.2.12479018, and CMake 3.22.1:

~~~bash
gradle :quest:assembleDebug
~~~

APK output:

    quest/build/outputs/apk/debug/quest-debug.apk

Install by ADB:

~~~bash
adb install -r quest/build/outputs/apk/debug/quest-debug.apk
~~~

The GitHub Actions artifact is named:

    DeskXR-Quest-debug

## Windows launcher

The Windows driver artifact now includes `DeskXR.ps1`, a small launcher/config tool for the common setup tasks:

- install/update the SteamVR driver;
- optionally create the UDP 39742 firewall rule;
- show likely LAN IPv4 addresses for the Quest app;
- start the Quest bridge directly over ADB with the selected PC LAN address;
- restore Quest proximity behavior after testing;
- choose full-size monocular desktop output or stereo side-by-side;
- set virtual HMD height and mouse-look sensitivity;
- copy the primary monitor resolution into DeskXR display settings;
- start SteamVR.

Run it with:

~~~powershell
powershell -ExecutionPolicy Bypass -File .\DeskXR.ps1
~~~

## Quest unworn mode

Meta documents that the Quest proximity sensor normally puts the headset to sleep as soon as it is removed, which pauses the OpenXR runtime. For DeskXR development/testing, the Windows artifact includes a helper that asks Horizon OS to behave as if the headset is still worn:

~~~powershell
powershell -ExecutionPolicy Bypass -File .\quest-unworn-mode.ps1
~~~

Restore normal proximity behavior when finished:

~~~powershell
powershell -ExecutionPolicy Bypass -File .\quest-unworn-mode.ps1 -Disable
~~~

This requires an authorized ADB connection to the Quest.

For a mostly headset-free startup, the packaged launcher can now do both steps at once. **Start Quest bridge (ADB)** enables the unworn override, launches `org.sakus.deskxr/.MainActivity`, passes the selected PC IPv4 address/UDP port through Android intent extras, and asks the app to start the OpenXR bridge automatically. **Stop Quest bridge + restore** force-stops DeskXR and returns proximity handling to the physical sensor.

The raw helper is:

~~~powershell
powershell -ExecutionPolicy Bypass -File .\quest-start.ps1 -HostAddress 192.168.1.20
~~~

## First real Quest test

1. Build/install and register the Windows driver.
2. Restart SteamVR.
3. Make sure Windows Firewall allows inbound UDP 39742.
4. Install the DeskXR APK on Quest.
5. Either launch it normally and enter the PC LAN IPv4 address, or use **Start Quest bridge (ADB)** from the Windows launcher.
6. Start VRChat in VR mode.

The Quest client locates each controller relative to OpenXR's VIEW reference space and applies a configurable mount transform. The UI lets you set virtual head height, Quest-to-head distance, vertical offset, and whether the Quest cameras face the user. The facing-user preset applies the 180-degree yaw needed for a typical monitor-mounted Quest.

A **Quick calibrate** button is also available after the bridge starts. Press it, then during the 3-second countdown hold both controllers shoulder-width apart in front of your upper chest. DeskXR estimates user yaw from the left-to-right controller vector and solves the translation that places the controller midpoint at a chest-level reference point. This replaces the manual transform for the current session.

You can also calibrate without looking at the Quest UI: hold the controllers in that same pose, squeeze both triggers past ~80%, click both thumbsticks, and keep the chord held for about 1.2 seconds. Both controllers pulse when calibration succeeds.

## Important hardware experiment

By default, Quest sleeps when the proximity sensor says the headset is not being worn, which pauses the OpenXR runtime. DeskXR now includes the ADB-based unworn helper above so this can be explicitly overridden during testing.

The APK reports the current OpenXR session state, packet rate, PC acknowledgement state, round-trip time, and received haptic count. `PC linked` means the SteamVR driver is receiving Quest packets and replying over the same UDP socket.

## UDP packet v1

The packet is little-endian and fixed-size. It contains a `DXR1` header plus left/right hand records with:

- position XYZ;
- quaternion XYZW;
- trigger;
- grip;
- thumbstick XY;
- button bitmask;
- pose-valid flags.

The PC driver considers controller data stale after one second and reports tracking loss instead of leaving frozen hands behind.

## Next milestones

- hardware test on Quest 3S with the documented unworn ADB override enabled;
- validate and tune the new guided two-controller calibration on real hardware;
- validate haptics on real Quest Touch hardware;
- validate mouse-look behavior inside VRChat;
- validate the new mono desktop compositor mode on SteamVR/VRChat;
- OSC FBT setup helper.

See `docs/architecture.md` for more detail.

## License

No license has been selected yet.
