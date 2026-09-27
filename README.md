# DeskXR

DeskXR is an experimental **HMD-less desktop VR bridge**.

The goal is to keep VRChat in VR mode while you look at a normal PC monitor, use Quest Touch controllers as real 6DoF hands, and optionally let VRChat receive OSC full-body trackers at the same time.

## Current end-to-end PoC

The repository now contains both halves of the first working pipeline:

- **Windows / SteamVR driver**
  - fixed virtual HMD;
  - left/right virtual controllers;
  - UDP input for 6DoF pose and controller actions;
  - trigger, grip, thumbstick, A/B/X/Y, menu;
  - stale-packet tracking loss;
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

## First real Quest test

1. Build/install and register the Windows driver.
2. Restart SteamVR.
3. Make sure Windows Firewall allows inbound UDP 39742.
4. Install and launch the DeskXR APK on Quest.
5. Enter the PC's LAN IPv4 address, for example 192.168.1.20.
6. Press **Start bridge**.
7. Start VRChat in VR mode.

The Quest client locates each controller relative to OpenXR's VIEW reference space and applies a configurable mount transform. The UI lets you set virtual head height, Quest-to-head distance, vertical offset, and whether the Quest cameras face the user. The facing-user preset applies the 180-degree yaw needed for a typical monitor-mounted Quest.

## Important hardware experiment

The key unknown is still Quest lifecycle behavior when the headset is **not being worn**.

DeskXR needs the OpenXR session and Touch controller tracking to remain active while the Quest is sitting on or above the monitor. If Horizon OS suspends the session because the proximity sensor says the headset is unworn, the next milestone will need a different lifecycle strategy.

The APK reports the current OpenXR session state, packet rate, PC acknowledgement state, and received haptic count so this can be tested immediately on real hardware. `PC linked` means the SteamVR driver is receiving Quest packets and replying over the same UDP socket.

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

- hardware test on Quest 3S while unworn;
- refine the current manual mount calibration into a guided calibration flow;
- validate haptics on real Quest Touch hardware;
- optional mouse-driven virtual head yaw/pitch;
- installer / launcher;
- OSC FBT setup helper.

See `docs/architecture.md` for more detail.

## License

No license has been selected yet.
