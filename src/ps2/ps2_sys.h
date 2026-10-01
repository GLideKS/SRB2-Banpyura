// Internal interfaces between the PS2 system-layer files (i_system.c, i_joy.c, i_main.c).
#ifndef PS2_SYS_H
#define PS2_SYS_H

#include "../doomtype.h"

// i_joy.c
void PS2Joy_Poll(void);        // both pads: read libpad, post engine events (called from I_OsPolling)
void PS2Joy_Shutdown(void);    // neutral events for everything held, pads released
void PS2Joy_WaitStart(void);   // block until Start is pressed (or the power button): fatal error screens

// i_video.c (optional): called by I_Error with the final message. A weak reference: if the video
// layer does not provide it the message only goes to the console log.
void PS2Video_FatalError(const char *message) __attribute__((weak));

#endif
