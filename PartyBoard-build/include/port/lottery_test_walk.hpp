#pragma once

#include <array>
#include <cstdint>

namespace partyboard::test {

// Scripted controller input only. TicketUpdate normalizes any nonzero stick
// direction to eight screen pixels; a repeating circle scratches only a ring.
// Anchor at the upper-left boundary, then sweep overlapping rows of the card.
constexpr std::array<std::int8_t, 2> lotteryScratchStick(unsigned frame)
{
    constexpr unsigned anchorFrames = 48;
    constexpr unsigned rowFrames = 26;
    constexpr unsigned rows = 13;
    const unsigned phase = frame % (anchorFrames + rowFrames * rows);
    if (phase < anchorFrames) return { -127, 127 };
    const unsigned sweep = phase - anchorFrames;
    if (sweep % rowFrames == rowFrames - 1) return { 0, -127 };
    return { static_cast<std::int8_t>((sweep / rowFrames) % 2 ? -127 : 127), 0 };
}

} // namespace partyboard::test
