#pragma once

#include <cstdint>

namespace deskxr::protocol {

inline constexpr char kMagic[4] = {'D', 'X', 'R', '1'};
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kPoseValid = 1u << 0;

enum Button : std::uint32_t {
    ButtonA = 1u << 0,
    ButtonB = 1u << 1,
    ButtonX = 1u << 2,
    ButtonY = 1u << 3,
    ButtonThumbstick = 1u << 4,
    ButtonMenu = 1u << 5,
};

#pragma pack(push, 1)

struct HandV1 {
    float position[3];
    float orientation[4]; // x, y, z, w
    float trigger;
    float grip;
    float joystick[2];
    std::uint32_t buttons;
    std::uint32_t flags;
};

struct PacketV1 {
    char magic[4];
    std::uint16_t version;
    std::uint16_t size;
    std::uint32_t sequence;
    HandV1 left;
    HandV1 right;
};

#pragma pack(pop)

static_assert(sizeof(HandV1) == 52);
static_assert(sizeof(PacketV1) == 116);

} // namespace deskxr::protocol
