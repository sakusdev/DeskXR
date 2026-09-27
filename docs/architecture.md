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

## Tracking spaces

For an HMD-less setup, the Quest itself is expected to be placed somewhere near the monitor and used as a tracking camera/base.

DeskXR uses two OpenXR reference spaces:

- **VIEW** for the manual startup transform. Controller coordinates are relative to the physical Quest pose, which makes the simple measured camera-to-user offsets intuitive;
- **LOCAL** after guided calibration. LOCAL is gravity-aligned, so placing the Quest on top of a monitor with some physical pitch/roll no longer tilts the user's controller coordinate frame.

If the OpenXR runtime reports that LOCAL space is being recentered, DeskXR invalidates the guided calibration and falls back until a new solve is captured.

The Quest UI exposes virtual head height, Quest-to-head distance, Quest vertical offset, and a facing-user preset for the manual fallback. The preset applies a 180-degree yaw before translating the controller pose into the virtual head frame.

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

Quest normally sleeps when its proximity sensor reports that it is not being worn, which pauses the OpenXR runtime.

DeskXR includes a development helper using Meta's documented ADB proximity override. The Windows launcher can enable that override and remotely launch the Quest app with PC host/port extras, allowing a mostly headset-free startup.

The APK reports OpenXR session state, outgoing packet rate, acknowledgement state, RTT, and haptic count so the remaining lifecycle behavior can be validated on real hardware.

## Desktop display modes

The virtual HMD exposes an OpenVR `IVRDisplayComponent` backed by a normal desktop window.

DeskXR supports two viewport layouts:

- **desktop mono** (default): both eyes target the same full-size desktop viewport. The second compositor eye effectively occupies the same monitor area, which avoids the squeezed side-by-side presentation and is intended for HMD-less monitor use;
- **stereo SBS**: left and right eyes use separate half-width viewports.

The window size is configurable and the Windows launcher can copy the current primary monitor resolution into `default.vrsettings`.

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

1. validate Quest 3S unworn tracking with the documented ADB override;
2. validate and tune the LOCAL-space guided calibration on real hardware;
3. validate bidirectional haptics and capacitive gestures in VRChat;
4. validate the monocular desktop compositor layout in SteamVR/VRChat;
5. validate mouse head rotation in VRChat and refine desktop controls based on real use.
