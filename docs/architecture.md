# DeskXR architecture

## Goal

DeskXR keeps the application in VR mode while the user looks at a normal monitor.

    Quest Touch L/R
          |
      OpenXR poses
          |
          v
    Quest client (planned)
          |
       UDP v1
          |
          v
    DeskXR SteamVR driver
       /             \
 virtual HMD      virtual hands
       \             /
            SteamVR
               |
             VRChat

OSC FBT intentionally stays outside this bridge. VRChat can receive OSC trackers directly; DeskXR only needs to supply the virtual HMD and hands.

## Milestone 0 — PC proof of concept

Implemented now:

1. virtual HMD with a fixed pose;
2. two virtual controller devices;
3. binary UDP input;
4. stale-packet handling;
5. controller simulator;
6. packaged Windows CI artifact.

## Milestone 1 — Quest OpenXR client

The Quest app should create an OpenXR session, bind the Touch interaction profile, locate left/right grip spaces each frame, read controller actions, transform poses into DeskXR tracking space, and send PacketV1 at roughly the tracking rate.

No PC video decoder is required.

The major unresolved platform question is lifecycle behavior when the Quest is physically not worn. That must be tested on-device.

## Milestone 2 — calibration

Quest local space and the virtual HMD space need a rigid transform.

A first calibration can ask the user to hold one controller at a known position relative to the monitor. A later flow can use both controllers to establish translation and yaw robustly.

## Milestone 3 — head modes

Planned head modes:

- fixed;
- mouse yaw/pitch;
- FBT-assisted position plus mouse rotation;
- optional external pose input.

Face tracking is not required.

## Packet v1

PacketV1 is fixed-size and little-endian. It contains a header and exactly two hands.

The driver treats packets older than one second as stale. The controller remains connected but reports an invalid pose instead of freezing silently.
