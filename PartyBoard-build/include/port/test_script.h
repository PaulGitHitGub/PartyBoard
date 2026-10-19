#ifndef PARTYBOARD_PORT_TEST_SCRIPT_H
#define PARTYBOARD_PORT_TEST_SCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

// Runs the timed test steps in PARTYBOARD_TEST_SCRIPT (HD beta; src/port/test_script.cpp).
// Called once per frame; does nothing otherwise.
void PartyBoard_TestScriptFrame(void);

#ifdef __cplusplus
}
#endif

#endif
