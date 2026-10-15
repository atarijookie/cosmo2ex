// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/select.h>
#include <sys/inotify.h>
#include <limits.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>

#include "../misc/global.h"
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "../misc/settings.h"
#include "../chipinterface/chipinterface.h"

#include "ikbd.h"

extern TInputDevice ikbdDevs[INTYPE_COUNT];

Ikbd::Ikbd(ChipInterface* ciIkbd)
{
    this->ciIkbd = ciIkbd;

    loadSettings();

    initDevs();

    keyJoyKeys.setKeyTranslator(&keyTranslator);        // first set the translator
    keyJoyKeys.loadKeys();                              // load the keys used for keyb joys

    mouseBtnNow = 0;

    // init per client state (buffers, modes)
    for(int i=0; i<MAX_CLIENTS; i++) {
        clientState[i].reset();
    }
    noClientState.reset();
    cur             = &noClientState;
    curClientIndex  = -1;

    fillSpecialCodeLengthTable();
    fillStCommandsLengthTable();

    resetInternalIkbdVars();
}

void Ikbd::loadSettings(void)
{
    Settings s;
    firstJoyIs0 = s.getBool("JOY_FIRST_IS_0", false);

    if(firstJoyIs0) {
        joy1st = INTYPE_JOYSTICK1;
        joy2nd = INTYPE_JOYSTICK2;
    } else {
        joy1st = INTYPE_JOYSTICK2;
        joy2nd = INTYPE_JOYSTICK1;
    }

    // get enabled flags for mouse wheel as keys
    mouseWheelAsArrowsUpDown = s.getBool("MOUSE_WHEEL_AS_KEYS", true);

    // get enabled flags for keyb joys
    keybJoy0 = s.getBool("KEYBOARD_JOY0", false);
    keybJoy1 = s.getBool("KEYBOARD_JOY1", false);

    keyJoyKeys.setKeyTranslator(&keyTranslator);        // first set the translator
    keyJoyKeys.loadKeys();                              // load the keys used for keyb joys
}

void Ikbd::resetInternalIkbdVars(void)
{
    cur->outputEnabled = true;                  // IKBD reset from this ST resumes its output

    mouseMode       = MOUSEMODE_REL;
    mouseEnabled    = true;
    mouseY0atTop    = true;
    mouseAbsBtnAct  = MOUSEBTN_REPORT_NOTHING;

    absMouse.maxX       = 640;
    absMouse.maxY       = 400;
    absMouse.x          = 0;
    absMouse.y          = 0;
    absMouse.buttons    = 0;
    absMouse.scaleX        = 1;
    absMouse.scaleY        = 1;
    absMouse.fracX         = 0;
    absMouse.fracY         = 0;

    relMouse.threshX    = 1;
    relMouse.threshY    = 1;
    relMouse.accX       = 0;
    relMouse.accY       = 0;

    keycodeMouse.deltaX    = 1;
    keycodeMouse.deltaY    = 1;
    keycodeMouse.accX      = 0;
    keycodeMouse.accY      = 0;

    joystickMode    = JOYMODE_EVENT;
    joystickState   = EnabledInMouseMode;

    resetHotkeyState();
}

void Ikbd::resetHotkeyState(void)
{
    leftShiftsPressed  = 0;
    rightShiftsPressed = 0;
    ctrlsPressed       = 0;
    f11sPressed        = 0;
    f12sPressed        = 0;
    waitingForHotkeyRelease = false;
    pressedKeys.reset();
}

void Ikbd::clientConnected(int clientIndex)
{
    if(clientIndex < 0 || clientIndex >= MAX_CLIENTS) {
        return;
    }

    clientState[clientIndex].reset();       // don't inherit half-received data / pause state of previous device in this slot
}

void Ikbd::sendToSt(const uint8_t* bfr, int len)
{
    if(curClientIndex >= 0) {               // processing data from one device? replies, its keyboard data etc. go only to that ST
        sendToClient(curClientIndex, bfr, len);
        return;
    }

    for(int i=0; i<MAX_CLIENTS; i++) {      // otherwise (USB keyboard / mouse / joystick) it goes to all connected STs
        sendToClient(i, bfr, len);
    }
}

void Ikbd::sendToClient(int clientIndex, const uint8_t* bfr, int len)
{
    ClientInfo* ci = ciIkbd->clientGetByIndex(clientIndex);

    if(!ci || ci->fdClient == FD_EMPTY) {   // no client here? skip it
        return;
    }

    IkbdClientState &st = clientState[clientIndex];

    if(!st.outputEnabled) {                 // ST paused the output? queue it, it's sent when ST resumes (any other command)
        for(int i=0; i<len; i++) {
            st.pausedOutput.add(bfr[i]);    // drops data when queue is full, like real IKBD with its small buffer
        }
        return;
    }

    ciIkbd->sendDataToChip(ci->fdClient, (uint8_t *) bfr, len);    // closes client on failure
}
