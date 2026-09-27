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
- reverse haptic packets back to the most recent valid Quest peer;
- periodic acknowledgement packets so the Quest can verify the PC link;
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
9. receives PC acknowledgement and haptic packets on the same UDP socket;
10. maps SteamVR haptic events to an OpenXR vibration-output action;
11. submits zero composition layers because DeskXR does not stream PC video to Quest.

The Android UI only configures the PC IPv4 address and UDP port.

## Why VIEW space

For an HMD-less setup, the Quest itself is expected to be placed somewhere near the monitor and used as a tracking camera/base.

Using VIEW space makes the controller packet relative to the current Quest headset pose.

The current Quest UI now applies a configurable camera-to-user transform. It exposes virtual head height, Quest-to-head distance, Quest vertical offset, and a facing-user preset. The preset applies a 180-degree yaw before translating the controller pose into the virtual head frame, which is the expected geometry for a Quest mounted on or above a monitor with its cameras looking at the user.

## Calibration

The mapping is a rigid transform:

    P_steamvr = T_quest_to_deskxr * P_quest

and the same yaw rotation is applied to controller orientation.

The manual mount transform remains as the startup fallback. For a typical monitor mount, the default model is:

- virtual head height: 1.65 m;
- Quest cameras facing the user: enabled;
- Quest distance in front of the virtual head: 0.70 m;
- Quest vertical offset from the virtual head: -0.30 m.

A guided runtime calibration is now implemented. The user starts a three-second countdown and then holds the controllers shoulder-width apart in front of the upper chest.

DeskXR computes:

1. the horizontal left-to-right controller vector;
2. a yaw that rotates that vector onto user-space +X;
3. the midpoint of the two controllers;
4. a translation that maps that midpoint to a chest-level anchor about 0.30 m below and 0.35 m forward of the virtual head.

This provides a full yaw + XYZ translation transform without requiring the user to measure the Quest mount. The chest anchor is still an anthropometric approximation, so real-hardware validation and optional fine adjustment remain useful.

The guided solve can be requested from the Android UI or entirely from the controllers. Holding both triggers above roughly 80% while clicking both thumbsticks for about 1.2 seconds performs the solve immediately; a short haptic pulse on both controllers confirms success. This avoids depending on the Android UI remaining visible after the OpenXR session takes over the headset display.

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

The PC virtual HMD keeps a fixed position, but mouse yaw/pitch is now implemented.

- F8 toggles global mouse-look capture;
- F9 resets yaw/pitch;
- the driver recenters the desktop cursor while capture is active;
- sensitivity and pitch limit come from the DeskXR driver settings.

Possible later head modes still include:

- FBT-assisted position plus mouse rotation;
- optional external pose input.

Face tracking is not required.

## OSC FBT

DeskXR does not need to convert OSC trackers into SteamVR trackers.

Once VRChat is running in VR mode through DeskXR, an existing OSC FBT sender can continue sending hip/chest/knee/foot tracker poses directly to VRChat.

This keeps the bridge small and avoids duplicating VRChat's OSC tracker path.

## UDP protocol v1

DeskXR keeps tracking and reverse-control packets small, fixed-size, and little-endian.

Tracking `PacketV1` uses magic `DXR1` and contains a header plus exactly two hands.

Each hand contains:

- XYZ position;
- XYZW orientation quaternion;
- trigger;
- grip;
- thumbstick XY;
- button bit mask;
- pose-valid flags.

The PC driver treats tracking packets older than one second as stale. The controller remains connected but reports an invalid pose instead of freezing silently.

The reverse path uses:

- `DXA1` acknowledgement packets, sent periodically by the PC with the last tracking sequence number;
- `DXH1` haptic packets, carrying hand, duration, frequency, and amplitude.

Because the PC replies to the source endpoint of the latest valid `DXR1` packet, the Quest does not need a separate listening-port setting.

## Next milestones

1. install the APK on Quest 3S and test unworn session behavior;
2. validate and tune the guided two-controller calibration on Quest 3S;
3. validate bidirectional haptics on real Quest Touch hardware;
4. validate mouse head rotation in VRChat and decide whether to expose it through a launcher instead of global hotkeys;
5. turn the current self-installing Windows artifact into a small launcher/config UI.
