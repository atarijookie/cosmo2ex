#ifndef _UI_H_
#define _UI_H_

// Screen of the audio player (CE_AUDIO, and CE_VIDEO for files without video): text in the desktop's resolution
// (40 or 80 columns), buttons, a position bar and the keys below. Keyboard only.

#include <stdint.h>

void    uiStart(const char *path);  // draws it all
void    uiUpdate(void);             // time, bar, state - only what changed
void    uiEnd(void);                // clears the screen

#endif
