// Session-log descriptions of what drives each controller port (0-7), for the
// 8-player party's "[8p] input" lines (src/game/pad.c). Read-only.

#include "port/pad_ports.h"
#include "port/test_input.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

bool keyboard_has_keys(unsigned port)
{
    u32 count = 0;
    if (const PADKeyButtonBinding *keys = PADGetKeyButtonBindings(port, &count)) {
        if (std::any_of(keys, keys + count, [](const PADKeyButtonBinding &b) { return b.scancode != PAD_KEY_INVALID; })) {
            return true;
        }
    }
    if (const PADKeyAxisBinding *axes = PADGetKeyAxisBindings(port, &count)) {
        if (std::any_of(axes, axes + count, [](const PADKeyAxisBinding &b) { return b.scancode != PAD_KEY_INVALID; })) {
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" int PartyBoard_PadPortDescribe(unsigned port, char *buf, unsigned size)
{
    std::string text;
    auto add = [&text](const std::string &part) {
        if (!text.empty()) {
            text += " + ";
        }
        text += part;
    };
    if (port < PAD_MAX_CONTROLLERS) {
        if (const char *name = PADGetName(port)) {
            add(std::string("controller '") + name + "'");
        }
        if (keyboard_has_keys(port)) {
            add("keyboard");
        }
        const unsigned phones = PartyBoard_PhoneControllerPads();
        if ((phones >> port) & 1u) {
            add("phone");
        } else if ((PartyBoard_TestInputHeldPads() >> port) & 1u) {
            add("test script");
        }
    }
    const bool any = !text.empty();
    if (!any) {
        text = "nothing";
    }
    if (buf != nullptr && size > 0) {
        std::snprintf(buf, size, "%s", text.c_str());
    }
    return any ? 1 : 0;
}
