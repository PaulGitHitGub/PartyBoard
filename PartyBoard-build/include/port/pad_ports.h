#ifndef PARTYBOARD_PORT_PAD_PORTS_H
#define PARTYBOARD_PORT_PAD_PORTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* What drives controller port 'port' (0-7) right now, for the session log:
   e.g. "controller 'Xbox Wireless Controller'", "keyboard", "phone",
   "test script", several joined with " + ", or "nothing". Always writes a
   terminated string. Returns 1 when something is connected, else 0. Reads
   only; never changes input state. */
int PartyBoard_PadPortDescribe(unsigned port, char *buf, unsigned size);

#ifdef __cplusplus
}
#endif

#endif
