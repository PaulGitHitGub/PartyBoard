#ifndef PARTYBOARD_PORT_SEND_LOGS_H
#define PARTYBOARD_PORT_SEND_LOGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* macOS: zip the latest logs and crash reports into Downloads and open the
 * Share window. Windows (send_logs_win.cpp): zip the newest sessions' logs
 * into Downloads, copy it to the iCloud Drive folder if there is one, and
 * show it in File Explorer. Returns immediately; the work runs in the
 * background. */
void PartyBoard_SendLogs(void);

#if defined(_WIN32)
#ifndef __cplusplus
#include <stdbool.h>
#endif
/* partyboard.exe --send-logs-self-test: builds a zip from made-up sessions in
 * %TEMP% and checks it byte for byte. */
bool PartyBoard_SendLogsRunSelfTest(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
