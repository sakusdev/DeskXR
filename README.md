# DeskXR

DeskXR is an experimental **HMD-less desktop VR bridge**.

The goal is simple: keep VRChat in VR mode while you look at a normal PC monitor, then use real 6DoF controllers for the hands. OSC full-body tracking can remain a separate direct input to VRChat.

## Current PoC

The initial PC-side implementation now includes:

- a SteamVR/OpenVR server driver;
- a fixed virtual HMD;
- left/right virtual controllers;
- UDP input for two 6DoF hand poses;
- trigger, grip, thumbstick and face-button input;
- SteamVR haptic events received and logged;
- a Python simulator for testing before the Quest client exists;
- Windows CI that packages a ready-to-register driver artifact.

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 C++ tools, CMake 3.20+, and SteamVR.

~~~powershell
cmake -S . -B build -A x64
cmake --build build --config Release
~~~

The packaged driver is written to:

    build/deskxr

Register it:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts/install-driver.ps1
~~~

Restart SteamVR after registering the driver.

## First test

Run:

~~~powershell
python tools/sim_sender.py
~~~

The simulator sends two moving hand poses to UDP port 39742. SteamVR should see one virtual HMD plus left/right controllers.

The HMD defaults to (0, 1.65, 0). Controller packets are expected in OpenVR-style tracking space: +X right, +Y up, -Z forward.

## UDP packet v1

The binary packet is little-endian and fixed-size. It contains a DXR1 header plus left/right hand records with position, quaternion, trigger, grip, joystick, button mask, and pose-valid flags.

The PC driver marks controller poses invalid if packets are stale for more than one second.

## Next milestone

The next step is a tiny Quest 3 / Quest 3S OpenXR client that reads Touch grip poses and actions and sends the same packet format. It does **not** need PC video streaming.

The main platform unknown is whether Quest keeps the OpenXR session and controller tracking alive while the headset is physically not worn. That needs an on-device test.

See docs/architecture.md for the planned path.

## License

No license has been selected yet.
