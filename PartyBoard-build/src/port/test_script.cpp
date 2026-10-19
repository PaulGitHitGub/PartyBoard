// Timed test steps without a test driver: what dev/testing/mgcap.py sends
// through the test input socket, read from the environment instead, so the
// same screens can be captured on Windows (which has no test input socket yet)
// and on the Mac. HD beta only; off unless PARTYBOARD_TEST_SCRIPT is set.
//
//   PARTYBOARD_TEST_SCRIPT="t=13:cap:r0 t=14:press:20:8 t=22:press:1000:8 t=27:cap:g1"
//   PARTYBOARD_TEST_SCRIPT_DIR=<folder>      where cap:<name> saves <name>.dds
//
// Times are seconds from the first frame. cap:<name> saves the next frame
// (convert with dev/testing/texconv.py); press:<hex buttons>[:frames] holds
// those buttons on pad 1 (6 frames by default) and then 4 neutral frames, like
// mgcap.py; hold:<hex>:<x>:<y>:<frames> holds buttons and stick. Buttons are
// the PADStatus mask: A=100, B=200, X=400, Y=800, START=1000, Z=10, R=20, L=40.
// quit closes the game, as closing its window does.
// Every step is logged ("[test-script]" lines).

#include "port/test_script.h"

#if defined(PARTYBOARD_HD_BETA)
#include <aurora/aurora.h>
#include <dolphin/pad.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Step {
    double seconds = 0;
    std::vector<std::string> fields; // kind and arguments
};

struct Press {
    PADStatus status {};
    int frames = 0;
};

std::vector<Step> gSteps;
size_t gNextStep = 0;
std::deque<Press> gPresses;
uint64_t gStart = 0;

PADStatus make_status(unsigned buttons, int x, int y)
{
    PADStatus status {};
    status.button = static_cast<u16>(buttons);
    status.stickX = static_cast<s8>(x < -128 ? -128 : x > 127 ? 127 : x);
    status.stickY = static_cast<s8>(y < -128 ? -128 : y > 127 ? 127 : y);
    status.analogA = (buttons & PAD_BUTTON_A) ? 0xFF : 0;
    status.analogB = (buttons & PAD_BUTTON_B) ? 0xFF : 0;
    status.triggerLeft = (buttons & PAD_TRIGGER_L) ? 0xFF : 0;
    status.triggerRight = (buttons & PAD_TRIGGER_R) ? 0xFF : 0;
    return status;
}

int number(const std::vector<std::string> &fields, size_t i, int base, int fallback)
{
    return i < fields.size() ? static_cast<int>(std::strtol(fields[i].c_str(), nullptr, base)) : fallback;
}

void load(const char *script)
{
    std::istringstream words(script);
    std::string word;
    while (words >> word) {
        // t=<seconds>:<kind>:<arguments...>
        if (word.rfind("t=", 0) != 0) {
            continue;
        }
        Step step;
        std::istringstream parts(word.substr(2));
        std::string part;
        std::getline(parts, part, ':');
        step.seconds = std::atof(part.c_str());
        while (std::getline(parts, part, ':')) {
            step.fields.push_back(part);
        }
        if (!step.fields.empty()) {
            gSteps.push_back(step);
        }
    }
    std::stable_sort(gSteps.begin(), gSteps.end(), [](const Step &a, const Step &b) { return a.seconds < b.seconds; });
    std::fprintf(stderr, "[test-script] %zu steps\n", gSteps.size());
}

void run(const Step &step)
{
    const std::string &kind = step.fields[0];
    if (kind == "cap" && step.fields.size() > 1) {
        const char *dir = std::getenv("PARTYBOARD_TEST_SCRIPT_DIR");
        const std::string path =
            (std::filesystem::path(dir != nullptr ? dir : ".") / (step.fields[1] + ".dds")).string();
        aurora_capture_next_frame(path.c_str());
        std::fprintf(stderr, "[test-script] t=%.1f capture %s\n", step.seconds, path.c_str());
    } else if (kind == "press" && step.fields.size() > 1) {
        const unsigned buttons = static_cast<unsigned>(number(step.fields, 1, 16, 0));
        gPresses.push_back({ make_status(buttons, 0, 0), number(step.fields, 2, 10, 6) });
        gPresses.push_back({ make_status(0, 0, 0), 4 });
        std::fprintf(stderr, "[test-script] t=%.1f press %x\n", step.seconds, buttons);
    } else if (kind == "hold" && step.fields.size() > 4) {
        const unsigned buttons = static_cast<unsigned>(number(step.fields, 1, 16, 0));
        gPresses.push_back({ make_status(buttons, number(step.fields, 2, 10, 0), number(step.fields, 3, 10, 0)),
            number(step.fields, 4, 10, 1) });
        std::fprintf(stderr, "[test-script] t=%.1f hold %x\n", step.seconds, buttons);
    } else if (kind == "quit") {
        SDL_Event quit {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
        std::fprintf(stderr, "[test-script] t=%.1f quit\n", step.seconds);
    } else {
        std::fprintf(stderr, "[test-script] t=%.1f unknown step %s\n", step.seconds, kind.c_str());
    }
}

} // namespace
#endif

extern "C" void PartyBoard_TestScriptFrame(void)
{
#if defined(PARTYBOARD_HD_BETA)
    static const char *script = std::getenv("PARTYBOARD_TEST_SCRIPT");
    if (script == nullptr) {
        return;
    }
    if (gStart == 0) {
        gStart = SDL_GetTicks() + 1;
        load(script);
    }
    const double seconds = static_cast<double>(SDL_GetTicks() + 1 - gStart) / 1000.0;
    while (gNextStep < gSteps.size() && gSteps[gNextStep].seconds <= seconds) {
        run(gSteps[gNextStep++]);
        if (gNextStep == gSteps.size()) {
            std::fprintf(stderr, "[test-script] done\n");
        }
    }
    if (!gPresses.empty()) {
        // A scripted press must reach the game even while one of PartyBoard's
        // own menus is open (they block pad input); test runs only.
        PADBlockInput(false);
        PADSetVirtualStatus(PAD_CHAN0, &gPresses.front().status);
        if (--gPresses.front().frames <= 0) {
            gPresses.pop_front();
            if (gPresses.empty()) {
                PADClearVirtualStatus(PAD_CHAN0);
            }
        }
    }
#endif
}
