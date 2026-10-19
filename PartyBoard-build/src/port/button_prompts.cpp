// Controller-matched button prompts.
//
// Mario Party 4 shows GameCube buttons in its messages and menus ("A Choose",
// "B Return", the X/Y/Z/L/R and stick icons). When player 1 uses an Xbox,
// PlayStation or Switch Pro controller, those icons are swapped for that
// controller's buttons, following its actual button mapping: GameCube A shows
// whichever button A is mapped to (Xbox A, PlayStation cross, Switch B by
// default). Only the pictures change; input is untouched. A GameCube
// controller or adapter, a keyboard, a phone controller or no controller keeps
// the original icons.
//
// The pictures (Contents/Resources/ButtonPrompts/<family>/<button>.png, drawn
// by tools/button-prompts/btnart.m) replace the game's icon textures through
// Aurora's texture replacement: the set in use is copied under the icon
// textures' names into the settings folder and registered as a directory.
// The same code runs on Windows, where the art sits in ButtonPrompts beside
// partyboard.exe; there the 3D textures and strips are put together by
// button_prompts_compose.cpp instead of button_prompts_compose.m.
//
// Every change of style is logged ("[prompts]" lines in game.log).

#include "button_prompts_tables.h"

#include <aurora/texture.hpp>
#include <dolphin/pad.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "port/main.h" // PartyBoard_ConfigPath

extern "C" int PartyBoard_ComposeButtonStrip(const char *outPath, int cellW, int cellH, int k,
    const char *const *glyphPaths, int count, int dimFrom, float dim);
extern "C" int PartyBoard_ComposeButtonTexture(const char *basePath, const char *labelPath, const char *outPath,
    int origWidth, float x0, float y0, float x1, float y1);

namespace {

// Paths go to the composers as UTF-8, the encoding SDL's base path comes in;
// on Windows a plain .string() would use the ANSI code page instead.
std::filesystem::path from_utf8(const char *text)
{
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(text)));
}

std::string to_utf8(const std::filesystem::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

const char *type_name(PADControllerType type)
{
    switch (type) {
    case PAD_TYPE_UNKNOWN: return "unknown";
    case PAD_TYPE_STANDARD: return "standard";
    case PAD_TYPE_XBOX360: return "Xbox 360";
    case PAD_TYPE_XBOXONE: return "Xbox One";
    case PAD_TYPE_PS3: return "PS3";
    case PAD_TYPE_PS4: return "PS4";
    case PAD_TYPE_PS5: return "PS5";
    case PAD_TYPE_SWITCH_PROCON: return "Switch Pro";
    case PAD_TYPE_JOYCON_LEFT: return "Joy-Con L";
    case PAD_TYPE_JOYCON_RIGHT: return "Joy-Con R";
    case PAD_TYPE_JOYCON_PAIR: return "Joy-Con pair";
    case PAD_TYPE_GAMECUBE: return "GameCube";
    case PAD_TYPE_NSO_GAMECUBE: return "NSO GameCube";
    }
    return "other";
}

// "xbox", "playstation", "switch", or empty for the GameCube originals.
std::string family_for(PADControllerType type, const PADButtonMapping *mappings, u32 count)
{
    switch (type) {
    case PAD_TYPE_STANDARD:
    case PAD_TYPE_XBOX360:
    case PAD_TYPE_XBOXONE:
        return "xbox";
    case PAD_TYPE_PS3:
    case PAD_TYPE_PS4:
    case PAD_TYPE_PS5:
        return "playstation";
    case PAD_TYPE_SWITCH_PROCON:
        // The Switch Online GameCube controller reports as a Pro Controller;
        // its mapping (B on the west button, X on the east) gives it away.
        if (native_for(mappings, count, PAD_BUTTON_B) == SDL_GAMEPAD_BUTTON_WEST
            && native_for(mappings, count, PAD_BUTTON_X) == SDL_GAMEPAD_BUTTON_EAST) {
            return {};
        }
        return "switch";
    case PAD_TYPE_JOYCON_LEFT:
    case PAD_TYPE_JOYCON_RIGHT:
    case PAD_TYPE_JOYCON_PAIR:
        return "switch";
    default:
        return {};
    }
}

// For the log: the picture a GameCube button shows, or "orig" when it keeps the original.
const char *glyph_or_orig(Slot slot, const PADButtonMapping *mappings, u32 count)
{
    const char *glyph = glyph_for(slot, mappings, count);
    return glyph != nullptr ? glyph : "orig";
}

std::string sActiveKey;
aurora::texture::ReplacementGroup sActiveGroup;
uint64_t sNextCheck = 0;

void apply(const std::string &family, const std::array<const char *, kTargets.size()> &glyphs, const std::string &key,
    bool defaultFaces, const PADButtonMapping *mappings, u32 count)
{
    aurora::texture::unregister_replacements(sActiveGroup);
    sActiveGroup = {};
    if (family.empty()) {
        std::fprintf(stderr, "[prompts] showing the original GameCube buttons\n");
        return;
    }
    const char *base = SDL_GetBasePath();
    if (base == nullptr) {
        std::fprintf(stderr, "[prompts] no program folder (%s); keeping the GameCube buttons\n", SDL_GetError());
        return;
    }
    const std::filesystem::path source = from_utf8(base) / "ButtonPrompts" / family;
    std::error_code dirError;
    if (!std::filesystem::is_directory(source, dirError)) {
        std::fprintf(stderr, "[prompts] art folder missing: %s; keeping the GameCube buttons\n", to_utf8(source).c_str());
        return;
    }
    const std::filesystem::path dir = PartyBoard_ConfigPath / "button-prompts" / key;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    for (size_t i = 0; i < kTargets.size(); ++i) {
        if (glyphs[i] == nullptr) {
            continue;
        }
        const std::string file = std::string(glyphs[i]) + (kTargets[i].wide ? "_wide.png" : ".png");
        // The sprite code looks these icons up before their palette is bound,
        // so the engine asks for them under a no-palette name as well
        // (tex1_WxH_<texture>_0000000000000000_9). Register both.
        const std::string name = kTargets[i].texture;
        std::vector<std::string> names { name };
        // tex1_WxH_<texture>_<palette>_<fmt>: palette textures only.
        if (std::count(name.begin(), name.end(), '_') == 4) {
            const auto paletteStart = name.find('_', name.find('_', name.find('_') + 1) + 1);
            names.push_back(name.substr(0, paletteStart + 1) + "0000000000000000" + name.substr(name.rfind('_')));
            // Some screens draw the same icon through another palette (the
            // L/R hint in the arm-wrestling duel); "$" matches any palette.
            names.push_back(name.substr(0, paletteStart + 1) + "$" + name.substr(name.rfind('_')));
        }
        std::filesystem::path made = source / file;
        if (kTargets[i].x1 != 0) {
            const Target &t = kTargets[i];
            made = dir / (name + ".png");
            if (!PartyBoard_ComposeButtonTexture(to_utf8(source.parent_path() / "3d" / (name + ".png")).c_str(),
                    to_utf8(source / "label" / file).c_str(), to_utf8(made).c_str(), t.origWidth, t.x0, t.y0, t.x1,
                    t.y1)) {
                std::fprintf(stderr, "[prompts] could not make %s with %s; it keeps the GameCube button\n", t.texture,
                    file.c_str());
                continue;
            }
        }
        for (const std::string &target : names) {
            if (dir / (target + ".png") != made) {
                std::filesystem::copy_file(made, dir / (target + ".png"), std::filesystem::copy_options::overwrite_existing,
                    ec);
            }
        }
    }
    for (const StripTarget &strip : kStrips) {
        std::vector<std::string> paths;
        std::vector<const char *> ptrs;
        for (int c = 0; c < strip.count; ++c) {
            const char *g = glyph_for(strip.cells[c], mappings, count);
            paths.push_back(g != nullptr ? to_utf8(source / (std::string(g) + ".png")) : std::string());
        }
        for (const auto &p : paths) {
            ptrs.push_back(p.empty() ? nullptr : p.c_str());
        }
        if (!PartyBoard_ComposeButtonStrip(to_utf8(dir / (std::string(strip.texture) + ".png")).c_str(), strip.cellW,
                strip.cellH, kStripScale, ptrs.data(), strip.count, strip.dimFrom, kStripDim)) {
            std::fprintf(stderr, "[prompts] could not make %s; it keeps the GameCube buttons\n", strip.texture);
        }
    }
    if (defaultFaces) {
        for (const char *bar : kBars) {
            std::filesystem::copy_file(source / "bars" / (std::string(bar) + ".png"), dir / (std::string(bar) + ".png"),
                std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    sActiveGroup = aurora::texture::load_replacement_directory(dir, { .priority = 10 });
    std::fprintf(stderr, "[prompts] %s buttons: %zu textures registered from %s\n", family.c_str(),
        sActiveGroup.registrations.size(), to_utf8(dir).c_str());
}

} // namespace

// Called once per main-loop pass; checks player 1's controller twice a second
// and swaps the icon set when the controller or its mapping changes.
extern "C" void PartyBoard_ButtonPromptsUpdate(void)
{
    const uint64_t now = SDL_GetTicks();
    if (now < sNextCheck) {
        return;
    }
    sNextCheck = now + 500;

    u32 count = 0;
    const PADButtonMapping *mappings = PADGetButtonMappings(0, &count);
    std::string family = family_for(PADGetControllerType(0), mappings, count);
    // Test and preview aid: PARTYBOARD_BUTTON_STYLE=xbox|playstation|switch|gamecube
    // forces a style; without a controller the default mapping is assumed.
    static const char *forced = std::getenv("PARTYBOARD_BUTTON_STYLE");
    if (forced != nullptr) {
        family = std::string(forced) == "gamecube" ? std::string() : std::string(forced);
        if (mappings == nullptr) {
            static const PADButtonMapping kDefault[] = {
                { SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A }, { SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_B },
                { SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X }, { SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y },
                { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z },
            };
            mappings = kDefault;
            count = sizeof(kDefault) / sizeof(kDefault[0]);
        }
    }
    std::array<const char *, kTargets.size()> glyphs {};
    std::string key = family.empty() ? "gamecube" : family;
    for (size_t i = 0; i < kTargets.size() && !family.empty(); ++i) {
        glyphs[i] = glyph_for(kTargets[i].slot, mappings, count);
        key += '-';
        key += glyphs[i] != nullptr ? glyphs[i] : "orig";
    }
    if (!family.empty() && native_for(mappings, count, PAD_BUTTON_A) == SDL_GAMEPAD_BUTTON_SOUTH
        && native_for(mappings, count, PAD_BUTTON_B) == SDL_GAMEPAD_BUTTON_EAST) {
        key += "-bars";
    }
    if (key == sActiveKey) {
        return;
    }
    sActiveKey = key;
    const PADControllerType type = PADGetControllerType(0);
    const char *name = PADGetName(0);
    std::fprintf(stderr, "[prompts] player 1: %s (type %d, %s)%s -> %s; A=%s B=%s X=%s Y=%s Z=%s L=%s R=%s Start=%s\n",
        name != nullptr ? name : "no controller", static_cast<int>(type), type_name(type),
        forced != nullptr ? " PARTYBOARD_BUTTON_STYLE" : "", family.empty() ? "gamecube" : family.c_str(),
        family.empty() ? "-" : glyph_or_orig(Slot::A, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::B, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::X, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::Y, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::Z, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::L, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::R, mappings, count),
        family.empty() ? "-" : glyph_or_orig(Slot::Start, mappings, count));
    const bool defaultFaces = native_for(mappings, count, PAD_BUTTON_A) == SDL_GAMEPAD_BUTTON_SOUTH
        && native_for(mappings, count, PAD_BUTTON_B) == SDL_GAMEPAD_BUTTON_EAST;
    apply(family, glyphs, family.empty() ? key : family, defaultFaces, mappings, count);
}
