# DeskXR architecture

## Goal

DeskXR keeps the application in VR mode while the user looks at a normal monitor.

    Quest Touch L/R
          |
       OpenXR
          |
          v
    DeskXR Quest client
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

## Implemented pipeline

### PC / SteamVR

The Windows driver currently provides:

- one fixed tracked HMD;
- two tracked controller devices;
- SteamVR Input components for trigger, grip, thumbstick, buttons and haptics;
- a UDP receiver on port 39742;
- one-second stale packet detection;
- a simulator for testing without Quest.

### Quest

The Android client is native OpenXR and arm64-only.

It currently:

1. initializes the Android OpenXR loader;
2. requests `XR_KHR_android_create_instance`;
3. creates a minimal OpenGL ES graphics binding;
4. creates an OpenXR session;
5. binds `/interaction_profiles/oculus/touch_controller`;
6. reads left/right grip poses and controller actions;
7. locates controller poses relative to `XR_REFERENCE_SPACE_TYPE_VIEW`;
8. sends DeskXR PacketV1 to the PC;
9. submits zero composition layers because DeskXR does not stream PC video to Quest.

The Android UI only configures the PC IPv4 address and UDP port.

## Why VIEW space

For an HMD-less setup, the Quest itself is expected to be placed somewhere near the monitor and used as a tracking camera/base.

Using VIEW space makes the controller packet relative to the current Quest headset pose. The current PoC then places that coordinate system around a fixed virtual head height of 1.65 m.

This is intentionally crude. A monitor-mounted Quest facing the user is not actually co-located or co-oriented with the user's head, so a real calibration transform is the next important tracking task.

## Calibration plan

The final mapping should be a rigid transform:

    P_steamvr = T_quest_to_deskxr * P_quest

and the same rotation component must be applied to controller orientation.

The first practical calibration UI should expose or solve:

- Quest/camera position relative to the virtual head;
- yaw between the Quest cameras and the user's forward direction;
- virtual head height.

A monitor-mounted Quest facing the user will usually need roughly 180 degrees of yaw plus a forward/back translation. Hard-coding only the current 1.65 m Y offset is not sufficient for accurate hands.

## OpenXR lifecycle experiment

The major hardware unknown is what Horizon OS does when the Quest is physically not worn.

DeskXR needs:

- the app to remain foreground/active;
- the OpenXR session to reach READY/FOCUSED;
- Touch tracking to remain active;
- the device not to sleep because of the proximity sensor.

The APK reports OpenXR session state and outgoing packet rate to make this easy to verify.

If the session is suspended while unworn, lifecycle handling becomes the next blocker before tracking quality.

## Head modes

The PC virtual HMD is currently fixed.

Possible later modes:

- fixed;
- mouse yaw/pitch;
- FBT-assisted position plus mouse rotation;
- optional external pose input.

Face tracking is not required.

## OSC FBT

DeskXR does not need to convert OSC trackers into SteamVR trackers.

Once VRChat is running in VR mode through DeskXR, an existing OSC FBT sender can continue sending hip/chest/knee/foot tracker poses directly to VRChat.

This keeps the bridge small and avoids duplicating VRChat's OSC tracker path.

## Packet v1

PacketV1 is fixed-size and little-endian. It contains a header and exactly two hands.

Each hand contains:

- XYZ position;
- XYZW orientation quaternion;
- trigger;
- grip;
- thumbstick XY;
- button bit mask;
- pose-valid flags.

The PC driver treats packets older than one second as stale. The controller remains connected but reports an invalid pose instead of freezing silently.

## Next milestones

1. install the APK on Quest 3S and test unworn session behavior;
2. implement camera-to-user calibration;
3. forward SteamVR haptics back to Quest;
4. add optional mouse head rotation;
5. package a one-click Windows installer/launcher.
