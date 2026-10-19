// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef _IKBDINPUTSINK_H_
#define _IKBDINPUTSINK_H_

#include "ikbd.h"
#include "inputsocket.h"

// delivers events from the input socket to Ikbd - same handlers as for evdev devices
class IkbdInputSink : public InputSink
{
public:
    IkbdInputSink(Ikbd& ikbd) : ikbd(ikbd) { }

    void keyboardEvent(input_event *ev) { ikbd.processKeyboard(ev, false); }   // false: don't skip keyboard translation

    void mouseEvent(input_event *ev) {
        ikbd.markInputMouseEventTime();     // mouse counts as present while its events come
        ikbd.processMouse(ev);
    }

    void joystickEvent(js_event *jse, int joyNumber) { ikbd.processJoystick(jse, joyNumber); }
    void joysticksPresent(bool joy0, bool joy1) { ikbd.setInputJoysticks(joy0, joy1); }

private:
    Ikbd& ikbd;
};

#endif
