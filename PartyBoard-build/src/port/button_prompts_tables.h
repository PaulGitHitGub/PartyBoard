// Controller-matched button prompts: which game textures are replaced and
// which controller button each one shows. Shared by button_prompts.cpp and
// tools/button-prompts/precompose.cpp (which draws, on a Mac, every 3D texture
// and strip cell the Windows build can need), so both use one list.
#pragma once

#include <dolphin/pad.h>

#include <SDL3/SDL_gamepad.h>

#include <array>

namespace {

enum class Slot { A, B, X, Y, Z, L, R, LPressed, RPressed, CStick, Stick, Start, DPad };

struct Target {
    Slot slot;
    const char *texture; // replacement file name of the game's icon, without .png
    bool wide;           // the 30x24 Z icon takes the 120x96 drawing
    // 3D button textures (x1 != 0): the label is composed at run time onto
    // ButtonPrompts/3d/<texture>.png inside this box (original texture pixels).
    short x0 = 0, y0 = 0, x1 = 0, y1 = 0, origWidth = 0;
};

// The message and menu icon set (WIN_ICON in the game data), one texture per
// button. Found with PARTYBOARD_DUMP_SPRITES.
constexpr std::array kTargets {
    // Message and menu icons (data/win.bin, entry 3).
    Target { Slot::A, "tex1_24x24_c4542d0403cfe570_79bad4c3f8c5cb4e_9", false },
    Target { Slot::B, "tex1_24x24_0f0e22a6b3ec2540_8dddd1be1be77ba5_9", false },
    Target { Slot::X, "tex1_24x24_e1f24c116b101e18_fc93e411da4da063_9", false },
    Target { Slot::Y, "tex1_24x24_d8a31c7cd8b7f4f1_8784d41fd6b635e6_9", false },
    Target { Slot::Z, "tex1_30x24_ce95c0f5204de46c_70d23bda06e420f1_9", true },
    Target { Slot::L, "tex1_24x24_56e6af620398f9f9_aba5a4e2b9c2ddda_9", false },
    Target { Slot::R, "tex1_24x24_bac5964166600690_c98b19911cde968b_9", false },
    // The second frame of the "press L/R" icons.
    Target { Slot::LPressed, "tex1_24x24_5508c97f0e307e44_053e23ee5585e4ff_9", false },
    Target { Slot::RPressed, "tex1_24x24_bde8231c0debe4e7_64781d8da4b21139_9", false },
    Target { Slot::CStick, "tex1_24x24_330729eaf2259844_2235b9935821f6fb_9", false },
    // The grey dome is the GameCube START button.
    Target { Slot::Start, "tex1_24x24_5f2c6c4c12dc441d_64a7fde2b5157129_9", false },
    Target { Slot::DPad, "tex1_24x24_196eb68db3a093bf_2d7370a31d54249e_9", false },
    // The control stick seen from the side ("Change", "Difficulty"), used with three palettes.
    Target { Slot::Stick, "tex1_24x24_79441a725feff2ab_0819d80f48cf8077_9", false },
    Target { Slot::Stick, "tex1_24x24_79441a725feff2ab_0f85acfa801c0d57_9", false },
    Target { Slot::Stick, "tex1_24x24_79441a725feff2ab_86c46c8d4ce7fa61_9", false },
    // Minigames with their own copies: m405 (Fish 'n' Drips) and m432.
    Target { Slot::A, "tex1_24x24_9405dc2dfa76739d_5", false },
    Target { Slot::B, "tex1_24x24_e36567af5346ad10_5", false },
    Target { Slot::L, "tex1_24x24_6a75a5b639903bd5_5", false },
    Target { Slot::R, "tex1_24x24_578519d2097a9a84_5", false },
    Target { Slot::R, "tex1_24x24_c0c8b29519611078_5", false },
    Target { Slot::Y, "tex1_24x24_b5e0b6a5d4329844_5", false },
    Target { Slot::X, "tex1_24x24_5679e8a3575175b5_5", false },
    // 3D buttons: the rules screen's START plate and Z bar, m418's A/B/X/Y
    // balls and m460's L/R key caps (tools/button-prompts/targets3d.txt).
    Target { Slot::Start, "tex1_128x128_b929d4f7a5f83f06_a6bdc4f6621677e7_9", true, 4, 34, 124, 96, 128 },
    Target { Slot::Z, "tex1_128x64_0acc07f7775b335a_fb787326932e90a8_9", true, 44, 3, 86, 41, 128 },
    Target { Slot::B, "tex1_64x64_0704e8a46aeda6ce_14", false, 12, 10, 52, 54, 64 },
    Target { Slot::A, "tex1_64x64_47c555db8c0cd62f_14", false, 12, 10, 52, 54, 64 },
    Target { Slot::X, "tex1_64x64_55f7271463119aa2_14", false, 12, 10, 52, 54, 64 },
    Target { Slot::Y, "tex1_64x64_f996c8f4fc2d8a80_14", false, 12, 10, 52, 54, 64 },
    Target { Slot::L, "tex1_128x128_e4db2443639012a4_4", false, 30, 22, 98, 92, 128 },
    Target { Slot::R, "tex1_128x128_9086cb5bf3c66e16_4", false, 30, 22, 98, 92, 128 },
};

// Images holding several buttons in a column, rebuilt from the glyphs: each
// cell is drawn at kStripScale times its original size, and the dimmed cells
// are darkened by kStripDim.
constexpr int kStripScale = 4;
constexpr float kStripDim = 0.35f;
struct StripTarget {
    const char *texture;
    short cellW, cellH, count, dimFrom;
    Slot cells[16];
};
const StripTarget kStrips[] = {
    // Fish 'n' Drips (m420): A B L R Y X Z, then the same seven dimmed.
    { "tex1_30x420_77262729cbccb1c4_5", 30, 30, 14, 7,
        { Slot::A, Slot::B, Slot::L, Slot::R, Slot::Y, Slot::X, Slot::Z,
          Slot::A, Slot::B, Slot::L, Slot::R, Slot::Y, Slot::X, Slot::Z } },
};

// Menu bars with buttons painted into the artwork (Mini-Game mode, Extra
// Room): replaced whole, from <family>/bars/<texture>.png, drawn for the
// default mapping only (A on the bottom face button, B on the right one).
constexpr std::array kBars {
    "tex1_216x36_994c24e6f5ffa5b3_351b4707fc250a31_9",
    "tex1_216x58_4fc5a36dd42f2f7d_14",
    "tex1_216x58_fa29a7f65a1aebb7_a32e580624f93fc5_9",
    "tex1_216x62_cbb6608a3ac153ca_93eaf50c52184af7_9",
    // The "Practice (Z) Back" banner shown while practising a minigame.
    "tex1_320x42_2a73cd9723bba377_8aa5321c3bc96777_9",
};

const char *native_button_name(u32 native)
{
    switch (native) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
    case SDL_GAMEPAD_BUTTON_EAST: return "east";
    case SDL_GAMEPAD_BUTTON_WEST: return "west";
    case SDL_GAMEPAD_BUTTON_NORTH: return "north";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "left_shoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "right_shoulder";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "left_stick";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "right_stick";
    default: return nullptr;
    }
}

u32 native_for(const PADButtonMapping *mappings, u32 count, PADButton button)
{
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        if (mappings[i].padButton == button) {
            return mappings[i].nativeButton;
        }
    }
    return PAD_NATIVE_BUTTON_INVALID;
}

// The picture for one icon, or nullptr to keep the GameCube original.
const char *glyph_for(Slot slot, const PADButtonMapping *mappings, u32 count);
const char *glyph_for(Slot slot, const PADButtonMapping *mappings, u32 count)
{
    switch (slot) {
    case Slot::A: return native_button_name(native_for(mappings, count, PAD_BUTTON_A));
    case Slot::B: return native_button_name(native_for(mappings, count, PAD_BUTTON_B));
    case Slot::X: return native_button_name(native_for(mappings, count, PAD_BUTTON_X));
    case Slot::Y: return native_button_name(native_for(mappings, count, PAD_BUTTON_Y));
    case Slot::Z: return native_button_name(native_for(mappings, count, PAD_TRIGGER_Z));
    // L and R are analogue triggers by default (no button mapped).
    case Slot::L: {
        const u32 native = native_for(mappings, count, PAD_TRIGGER_L);
        return native == PAD_NATIVE_BUTTON_INVALID ? "left_trigger" : native_button_name(native);
    }
    case Slot::R: {
        const u32 native = native_for(mappings, count, PAD_TRIGGER_R);
        return native == PAD_NATIVE_BUTTON_INVALID ? "right_trigger" : native_button_name(native);
    }
    case Slot::LPressed: return glyph_for(Slot::L, mappings, count);
    case Slot::RPressed: return glyph_for(Slot::R, mappings, count);
    case Slot::CStick: return "right_stick";
    case Slot::Stick: return "left_stick";
    case Slot::Start: {
        const u32 native = native_for(mappings, count, PAD_BUTTON_START);
        return native == SDL_GAMEPAD_BUTTON_START || native == PAD_NATIVE_BUTTON_INVALID ? "start" : native_button_name(native);
    }
    case Slot::DPad: return "dpad";
    }
    return nullptr;
}

} // namespace
