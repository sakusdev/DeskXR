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

Using VIEW space makes the controller packet relative to the current Quest headset pose.

The current Quest UI now applies a configurable camera-to-user transform. It exposes virtual head height, Quest-to-head distance, Quest vertical offset, and a facing-user preset. The preset applies a 180-degree yaw before translating the controller pose into the virtual head frame, which is the expected geometry for a Quest mounted on or above a monitor with its cameras looking at the user.

## Calibration

The mapping is a rigid transform:

    P_steamvr = T_quest_to_deskxr * P_quest

and the same yaw rotation is applied to controller orientation.

The first manual calibration is implemented. For a typical monitor mount, the default model is:

- virtual head height: 1.65 m;
- Quest cameras facing the user: enabled;
- Quest distance in front of the virtual head: 0.70 m;
- Quest vertical offset from the virtual head: -0.30 m.

These are only starting values. The later guided calibration flow should estimate the transform from one or more known controller poses instead of asking the user to tune numbers manually.

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
2. replace manual mount calibration with a guided calibration flow;
3. forward SteamVR haptics back to Quest;
4. add optional mouse head rotation;
5. package a one-click Windows installer/launcher.
