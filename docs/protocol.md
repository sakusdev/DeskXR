# DeskXR UDP protocol v1

DeskXR uses one UDP socket in each direction.

- Quest sends tracking/input packets to the PC driver on UDP 39742 by default.
- The PC remembers the source endpoint of the latest valid tracking packet.
- The PC sends acknowledgements and haptics back to that same endpoint.
- All structures are packed, fixed-size, and little-endian.

The canonical C++ layout is in `common/protocol.hpp`.

## Tracking — DXR1

Size: **116 bytes**

Header:

| Field | Type | Meaning |
| --- | --- | --- |
| magic | char[4] | `DXR1` |
| version | uint16 | `1` |
| size | uint16 | `116` |
| sequence | uint32 | wrapping tracking sequence |

The header is followed by exactly two 52-byte hand records: left, then right.

Hand record:

| Field | Type |
| --- | --- |
| position | float32[3] |
| orientation | float32[4], XYZW |
| trigger | float32 |
| grip | float32 |
| joystick | float32[2] |
| buttons | uint32 bit mask |
| flags | uint32 bit mask |

Tracking space follows OpenVR convention: +X right, +Y up, -Z forward, meters.

`flags & 1` means the pose is valid.

### Button bits

| Bit | Meaning |
| ---: | --- |
| 0 | A click |
| 1 | B click |
| 2 | X click |
| 3 | Y click |
| 4 | thumbstick click |
| 5 | menu click |
| 16 | A capacitive touch |
| 17 | B capacitive touch |
| 18 | X capacitive touch |
| 19 | Y capacitive touch |
| 20 | trigger capacitive touch |
| 21 | thumbstick capacitive touch |
| 22 | thumbrest capacitive touch |

Using high bits for capacitive state preserves the original 116-byte packet size.

## Acknowledgement — DXA1

Size: **12 bytes**

| Field | Type | Meaning |
| --- | --- | --- |
| magic | char[4] | `DXA1` |
| version | uint16 | `1` |
| size | uint16 | `12` |
| sequence | uint32 | tracking sequence being acknowledged |

The PC currently sends an acknowledgement roughly every 16 tracking packets. The Quest client treats an acknowledgement received within the last two seconds as `PC linked`.

## Haptic — DXH1

Size: **28 bytes**

| Field | Type | Meaning |
| --- | --- | --- |
| magic | char[4] | `DXH1` |
| version | uint16 | `1` |
| size | uint16 | `28` |
| sequence | uint32 | wrapping haptic sequence |
| hand | uint8 | 0 = left, 1 = right |
| reserved | uint8[3] | zero |
| durationSeconds | float32 | vibration duration |
| frequencyHz | float32 | 0 means runtime default |
| amplitude | float32 | clamped to 0..1 |

The Quest client maps this packet to an OpenXR vibration-output action for the corresponding Touch controller.

## Failure behavior

The PC treats tracking data as stale after one second. It keeps the virtual controller connected but reports an invalid pose and zeroes its inputs instead of leaving a frozen hand.

UDP is intentionally used without retransmission for pose data. A newer tracking packet always supersedes an older one. Haptic packets are currently best-effort as well.
