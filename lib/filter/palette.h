
#include <stdint.h>

// 16-color Pico-8 style palette
static const uint32_t PICO_PALETTE[16] = {
    0x000000, // 0 black
    0x1D2B53, // 1 dark-blue
    0x7E2553, // 2 dark-purple
    0x008751, // 3 dark-green
    0xAB5236, // 4 brown
    0x5F574F, // 5 dark-grey
    0xC2C3C7, // 6 light-grey
    0xFFF1E8, // 7 white
    0xFF004D, // 8 red
    0xFFA300, // 9 orange
    0xFFEC27, // 10 yellow
    0x00E436, // 11 green
    0x29ADFF, // 12 blue
    0x83769C, // 13 lavender
    0xFF77A8, // 14 pink
    0xFFCCAA  // 15 light-peach
};

static const uint32_t PALETTE_ELEVATE[] = {
    0x251c06, // 1  dark olive
    0xdfdec8, // 2  warm ivory
    0x5a300a, // 3  brown
    0xb6240a, // 4  brick red
    0x2a2d7a, // 5  deep blue
    0xe8ad84, // 6  peach
    0xf2e467, // 7  yellow
    0x2a8700, // 8  green
    0x090000, // 9  near black
    0x7ca7c0, // 10 desaturated sky
    0xd0c936, // 11 lime yellow
    0x184800, // 12 forest green
    0xa03dd0, // 13 purple
    0x70000e, // 14 burgundy
    0x3a1969, // 15 indigo
    0x4cc0d0  // 16 cyan
};

static const int PALETTE_ELEVATE_SIZE = 16;
