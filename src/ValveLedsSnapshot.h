#pragma once

#include <cstddef>
#include <cstdint>

constexpr unsigned int SteamLedCount = 17;
constexpr std::uint32_t ValveLedsMagic = 0x564c4544;
constexpr std::uint16_t ValveLedsVersion = 1;

enum ValveLedsEffect : std::uint8_t
{
    ValveLedsEffectOff = 0,
    ValveLedsEffectManual = 1,
    ValveLedsEffectNormal = 2,
    ValveLedsEffectRainbow = 3,
    ValveLedsEffectBreath = 4,
    ValveLedsEffectPatrol = 5,
};

#pragma pack(push, 1)
struct ValveLedsPixel
{
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t brightness;
};

struct ValveLedsSnapshot
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint64_t seq;
    std::uint64_t monotonic_ns;
    std::uint8_t enabled;
    std::uint8_t effect;
    std::uint8_t brightness_scale;
    std::uint8_t delay;
    std::uint8_t breath_offset;
    std::uint8_t breath_level;
    std::uint8_t patrol_num;
    std::uint8_t color_shift;
    ValveLedsPixel pixels[SteamLedCount];
};
#pragma pack(pop)

static_assert(sizeof(ValveLedsPixel) == 4, "valve-leds pixel layout changed");
static_assert(offsetof(ValveLedsSnapshot, seq) == 8, "valve-leds sequence offset changed");
static_assert(offsetof(ValveLedsSnapshot, pixels) == 32, "valve-leds pixel offset changed");
static_assert(sizeof(ValveLedsSnapshot) == 100, "valve-leds-shim UAPI size changed");
