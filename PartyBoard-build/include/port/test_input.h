#ifndef PARTYBOARD_PORT_TEST_INPUT_H
#define PARTYBOARD_PORT_TEST_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Applies scripted pad input when PARTYBOARD_TEST_INPUT is set (src/port/test_input.cpp).
// Called once per frame; does nothing otherwise.
void PartyBoard_TestInputFrame(void);

// For the Phone Controller settings: -2 input socket off, -1 the
// phone-controller bridge is not connected, otherwise phones connected.
int PartyBoard_PhoneControllerState(void);

// Pads with a phone on them, bit 0 = player 1 (up to 8 bits).
unsigned PartyBoard_PhoneControllerPads(void);

// Pads currently held by an "S" line on the input socket (phones or a test
// driver), bit 0 = pad 1 (up to 8 bits).
unsigned PartyBoard_TestInputHeldPads(void);

// Buttons an "S" line holds on pad `pad` (0-7) right now, 0 if none.
unsigned PartyBoard_TestInputPadButtons(int pad);

#ifdef __cplusplus
}
#endif

#endif
