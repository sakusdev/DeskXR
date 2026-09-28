# Quest unworn mode

DeskXR is unusual because the Quest headset is used as a controller-tracking base while the user looks at a PC monitor.

By default, Horizon OS uses the proximity sensor to put the headset to sleep when it is removed from the user's face. Meta's current developer documentation notes that this pauses the OpenXR runtime.

For development/testing, Meta documents an ADB broadcast that makes the device behave as though the proximity sensor is closed.

## Enable

Connect the Quest over USB, authorize ADB, then run:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts/quest-unworn-mode.ps1
~~~

Equivalent raw command:

~~~text
adb shell am broadcast -a com.oculus.vrpowermanager.prox_close
~~~

## Restore normal behavior

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts/quest-unworn-mode.ps1 -Disable
~~~

Equivalent raw command:

~~~text
adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable
~~~

Restore normal proximity handling when DeskXR testing is finished.

## What to verify

With DeskXR running and the headset placed on/above the monitor:

1. OpenXR state should remain active, preferably `FOCUSED`.
2. Packet rate should stay non-zero.
3. The Quest app should show `PC linked`.
4. Moving Touch controllers should move the SteamVR virtual hands.
5. SteamVR haptic events should increment the Quest haptic counter.

If OpenXR falls back to `VISIBLE` or another non-focused state, controller actions can stop even if the process is still alive.

## Source

Meta documents the proximity override in its current Quest developer tooling documentation under "Test with the headset off your face":

https://developers.meta.com/horizon/documentation/unity/meta-xr-operator/quest/
