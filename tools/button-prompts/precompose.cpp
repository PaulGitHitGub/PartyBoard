// precompose draw <art> <out>
//   Draws, with the Mac composer itself (src/port/button_prompts_compose.m),
//   every 3D button texture and strip cell the game can ask for, into
//   <out>/<family>/<texture>/ (installed on Windows as
//   ButtonPrompts/<family>/composed/<texture>/, see button_prompts_compose.cpp).
// precompose verify <ButtonPrompts> <random strips per family>
//   <ButtonPrompts> is the installed layout (art plus composed). Composes with
//   the Mac composer and with the Windows one (button_prompts_compose.cpp,
//   built in here too) and compares the decoded pixels: every 3D texture with
//   every label it can get, and strips for the default mapping, each single
//   button on every cell, and random mappings.
// Built and run by precompose.sh. Plain C++: the Mac composer is linked in.
#include "button_prompts_tables.h"

// The Windows composer, under other names so both can be linked here.
#define PartyBoard_ComposeButtonTexture Windows_ComposeButtonTexture
#define PartyBoard_ComposeButtonStrip Windows_ComposeButtonStrip
#include "button_prompts_compose.cpp"
#undef PartyBoard_ComposeButtonTexture
#undef PartyBoard_ComposeButtonStrip

#include <cstdlib>
#include <random>
#include <set>
#include <unistd.h>

extern "C" int PartyBoard_ComposeButtonTexture(const char *basePath, const char *labelPath, const char *outPath,
    int origWidth, float x0, float y0, float x1, float y1);
extern "C" int PartyBoard_ComposeButtonStrip(const char *outPath, int cellW, int cellH, int k,
    const char *const *glyphPaths, int count, int dimFrom, float dim);

namespace {

const char *const kFamilies[] = { "xbox", "playstation", "switch" };
const u32 kPadButtons[] = { PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_TRIGGER_Z, PAD_TRIGGER_L,
    PAD_TRIGGER_R, PAD_BUTTON_START };

// Every picture glyph_for can give this slot, for any mapping.
std::set<std::string> glyphs_for(Slot slot)
{
    std::set<std::string> glyphs;
    for (u32 native = 0; native <= SDL_GAMEPAD_BUTTON_COUNT; ++native) {
        PADButtonMapping mappings[std::size(kPadButtons)];
        for (size_t i = 0; i < std::size(kPadButtons); ++i) {
            mappings[i] = { native, static_cast<PADButton>(kPadButtons[i]) };
        }
        // native == SDL_GAMEPAD_BUTTON_COUNT stands for "nothing mapped".
        const u32 count = native == SDL_GAMEPAD_BUTTON_COUNT ? 0 : u32(std::size(kPadButtons));
        if (const char *glyph = glyph_for(slot, mappings, count)) {
            glyphs.insert(glyph);
        }
    }
    return glyphs;
}

std::string box_params(const Target &t)
{
    char params[128];
    std::snprintf(params, sizeof(params), "texture %d %g %g %g %g", int(t.origWidth), float(t.x0), float(t.y0),
        float(t.x1), float(t.y1));
    return params;
}

std::string strip_params(const StripTarget &s)
{
    char params[128];
    std::snprintf(params, sizeof(params), "strip %d %d %d %g", int(s.cellW), int(s.cellH), kStripScale, kStripDim);
    return params;
}

std::string label_file(const Target &t, const std::string &glyph)
{
    return glyph + (t.wide ? "_wide.png" : ".png");
}

void write_text(const fs::path &path, const std::string &text)
{
    std::ofstream(path) << text << "\n";
}

int fail(const std::string &what)
{
    std::fprintf(stderr, "precompose: %s\n", what.c_str());
    return 1;
}

int draw(const fs::path &art, const fs::path &out)
{
    int files = 0;
    for (const char *family : kFamilies) {
        for (const Target &t : kTargets) {
            if (t.x1 == 0) {
                continue;
            }
            const fs::path folder = out / family / t.texture;
            fs::create_directories(folder);
            write_text(folder / "params.txt", box_params(t));
            for (const std::string &glyph : glyphs_for(t.slot)) {
                const std::string file = label_file(t, glyph);
                if (!PartyBoard_ComposeButtonTexture(to_utf8(art / "3d" / (std::string(t.texture) + ".png")).c_str(),
                        to_utf8(art / family / "label" / file).c_str(), to_utf8(folder / file).c_str(), t.origWidth,
                        t.x0, t.y0, t.x1, t.y1)) {
                    return fail("cannot draw " + to_utf8(folder / file));
                }
                ++files;
            }
        }
        for (const StripTarget &s : kStrips) {
            const fs::path folder = out / family / s.texture;
            fs::create_directories(folder);
            write_text(folder / "params.txt", strip_params(s));
            std::set<std::string> glyphs;
            for (int c = 0; c < s.count; ++c) {
                const std::set<std::string> more = glyphs_for(s.cells[c]);
                glyphs.insert(more.begin(), more.end());
            }
            for (const std::string &glyph : glyphs) {
                const std::string path = to_utf8(art / family / (glyph + ".png"));
                const char *paths[] = { path.c_str() };
                // One cell: dimFrom 1 leaves it as is, dimFrom 0 darkens it.
                for (int dimFrom = 1; dimFrom >= 0; --dimFrom) {
                    const fs::path cell = folder / (glyph + (dimFrom == 0 ? "_dim.png" : ".png"));
                    if (!PartyBoard_ComposeButtonStrip(to_utf8(cell).c_str(), s.cellW, s.cellH, kStripScale, paths, 1,
                            dimFrom, kStripDim)) {
                        return fail("cannot draw " + to_utf8(cell));
                    }
                    ++files;
                }
            }
        }
    }
    std::printf("precompose: drew %d pictures into %s\n", files, to_utf8(out).c_str());
    return 0;
}

// Whether both files decode (as the game decodes them) to the same pixels.
bool same_pixels(const fs::path &a, const fs::path &b, std::string &why)
{
    std::vector<unsigned char> pa, pb;
    unsigned wa = 0, ha = 0, wb = 0, hb = 0;
    if (!load_rgba(a, pa, wa, ha) || !load_rgba(b, pb, wb, hb)) {
        why = "cannot decode";
        return false;
    }
    if (wa != wb || ha != hb) {
        why = "sizes differ";
        return false;
    }
    size_t differ = 0;
    for (size_t i = 0; i < pa.size(); i += 4) {
        differ += std::memcmp(&pa[i], &pb[i], 4) != 0;
    }
    if (differ != 0) {
        why = std::to_string(differ) + " pixels differ";
        return false;
    }
    return true;
}

int verify(const fs::path &prompts, int randomStrips)
{
    const fs::path tmp = fs::temp_directory_path() / ("precompose-verify-" + std::to_string(getpid()));
    fs::create_directories(tmp);
    // The strip composer finds its cells by the output's name: the texture's.
    fs::path mac, windows;
    auto outputs = [&](const char *texture) {
        mac = tmp / "mac" / (std::string(texture) + ".png");
        windows = tmp / "windows" / (std::string(texture) + ".png");
    };
    fs::create_directories(tmp / "mac");
    fs::create_directories(tmp / "windows");
    std::string why;
    int textures = 0, strips = 0, failures = 0;
    auto check = [&](int macOk, int windowsOk, const std::string &what) {
        if (!macOk || !windowsOk || !same_pixels(mac, windows, why)) {
            std::fprintf(stderr, "precompose: DIFFERENT %s (mac %d, windows %d) %s\n", what.c_str(), macOk, windowsOk,
                macOk && windowsOk ? why.c_str() : "");
            ++failures;
        }
        fs::remove(mac);
        fs::remove(windows);
    };
    for (const char *family : kFamilies) {
        const fs::path source = prompts / family;
        for (const Target &t : kTargets) {
            if (t.x1 == 0) {
                continue;
            }
            outputs(t.texture);
            const std::string base = to_utf8(prompts / "3d" / (std::string(t.texture) + ".png"));
            for (const std::string &glyph : glyphs_for(t.slot)) {
                const std::string label = to_utf8(source / "label" / label_file(t, glyph));
                const int macOk = PartyBoard_ComposeButtonTexture(base.c_str(), label.c_str(), to_utf8(mac).c_str(),
                    t.origWidth, t.x0, t.y0, t.x1, t.y1);
                const int windowsOk = Windows_ComposeButtonTexture(base.c_str(), label.c_str(),
                    to_utf8(windows).c_str(), t.origWidth, t.x0, t.y0, t.x1, t.y1);
                check(macOk, windowsOk, std::string(family) + " " + t.texture + " " + glyph);
                ++textures;
            }
        }
        for (const StripTarget &s : kStrips) {
            outputs(s.texture);
            // Mappings to try: the default, every picture on every cell, and random ones.
            std::vector<std::vector<std::string>> cases;
            std::vector<std::string> defaults;
            const PADButtonMapping kDefault[] = { { SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A },
                { SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_B }, { SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X },
                { SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z } };
            for (int c = 0; c < s.count; ++c) {
                const char *glyph = glyph_for(s.cells[c], kDefault, u32(std::size(kDefault)));
                defaults.push_back(glyph != nullptr ? glyph : "");
            }
            cases.push_back(defaults);
            for (int c = 0; c < s.count; ++c) {
                for (const std::string &glyph : glyphs_for(s.cells[c])) {
                    std::vector<std::string> one = defaults;
                    one[c] = glyph;
                    cases.push_back(one);
                }
            }
            std::mt19937 random(12345);
            for (int n = 0; n < randomStrips; ++n) {
                PADButtonMapping mappings[std::size(kPadButtons)];
                u32 count = 0;
                for (u32 button : kPadButtons) {
                    // Any SDL button, or (one time in ten) none.
                    if (random() % 10 != 0) {
                        mappings[count++] = { u32(random() % SDL_GAMEPAD_BUTTON_COUNT), static_cast<PADButton>(button) };
                    }
                }
                std::vector<std::string> glyphs;
                for (int c = 0; c < s.count; ++c) {
                    const char *glyph = glyph_for(s.cells[c], mappings, count);
                    glyphs.push_back(glyph != nullptr ? glyph : "");
                }
                cases.push_back(glyphs);
            }
            for (const std::vector<std::string> &glyphs : cases) {
                std::vector<std::string> paths;
                std::vector<const char *> ptrs;
                std::string what = std::string(family) + " " + s.texture;
                for (const std::string &glyph : glyphs) {
                    paths.push_back(glyph.empty() ? std::string() : to_utf8(source / (glyph + ".png")));
                    what += " " + (glyph.empty() ? std::string("-") : glyph);
                }
                for (const std::string &path : paths) {
                    ptrs.push_back(path.empty() ? nullptr : path.c_str());
                }
                const int macOk = PartyBoard_ComposeButtonStrip(to_utf8(mac).c_str(), s.cellW, s.cellH, kStripScale,
                    ptrs.data(), s.count, s.dimFrom, kStripDim);
                const int windowsOk = Windows_ComposeButtonStrip(to_utf8(windows).c_str(), s.cellW, s.cellH,
                    kStripScale, ptrs.data(), s.count, s.dimFrom, kStripDim);
                check(macOk, windowsOk, what);
                ++strips;
            }
        }
    }
    fs::remove_all(tmp);
    std::printf("precompose: verified %d 3D textures and %d strips: %d different\n", textures, strips, failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "draw") {
        return draw(from_utf8(argv[2]), from_utf8(argv[3]));
    }
    if (argc == 4 && std::string(argv[1]) == "verify") {
        return verify(from_utf8(argv[2]), std::atoi(argv[3]));
    }
    std::fprintf(stderr, "usage: precompose draw <art> <out> | verify <ButtonPrompts> <random strips>\n");
    return 2;
}
