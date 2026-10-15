// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#include "../misc/global.h"
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "../misc/settings.h"
#include "../chipinterface/chipinterface.h"
#include "ikbd.h"

static_assert(sizeof(input_event) <= INPUT_EVENT_MAX_SIZE && sizeof(js_event) <= INPUT_EVENT_MAX_SIZE, "TInputDevice.partial too small");

extern TInputDevice ikbdDevs[INTYPE_COUNT];
extern bool noCapture;

void Ikbd::initDevs(void)
{
    int i;

    lastVDevMouseEventTime = 0;

    keyboardExclusiveAccess = !noCapture;   // capture mode: grab keyboard & mouse when found (toggled by LCTRL+RCTRL+LALT+RALT)
    memset(keybJoyHeld, 0, sizeof(keybJoyHeld));

    for(i=0; i<INTYPE_COUNT; i++) {
        ikbdDevs[i].fd = -1;

        deinitDev(i);
    }
}

void Ikbd::deinitDev(int index)
{
    if(index < 0 || index >= INTYPE_COUNT) {    // out of index?
        return;
    }

    if(ikbdDevs[index].fd != -1) {            // device was open? close it
        close(ikbdDevs[index].fd);
    }

    ikbdDevs[index].fd            = -1;       // set vars to default init values
    ikbdDevs[index].devPath[0]    = 0;
    ikbdDevs[index].partialLen    = 0;

    if(index == INTYPE_KEYBOARD || index == INTYPE_MOUSE || index == INTYPE_VDEVKEYBOARD) {   // key source gone? releases of held keys won't come anymore
        resetHotkeyState();
    }

    if(index == INTYPE_JOYSTICK1) {         // for joy1 - init it
        initJoystickState(&joystick[0]);
    }

    if(index == INTYPE_JOYSTICK2) {         // for joy2 - init it
        initJoystickState(&joystick[1]);
    }
}

void Ikbd::initJoystickState(TJoystickState *joy)
{
    for(int i=0; i<JOYAXIS; i++) {
        joy->axis[i] = 0;
    }

    for(int i=0; i<JOYBUTTONS; i++) {
        joy->button[i] = 0;
    }

    joy->lastDir = 0;
    joy->lastBtn = 0;
}

void Ikbd::closeDevs(void)
{
    for(int i=0; i<INTYPE_COUNT; i++) {
        if(ikbdDevs[i].fd != -1) {        // if device is open
            close(ikbdDevs[i].fd);        // close it
            ikbdDevs[i].fd = -1;          // mark it as closed
        }
    }
}

void Ikbd::findDevices(void)
{
    char linkBuf[PATH_BUFF_SIZE];
    char devBuf[PATH_BUFF_SIZE];

    DIR *dir = opendir(INPUT_LINKS_PATH);                            // try to open the dir

    if(dir == NULL) {                                                 // not found?
        return;
    }

    while(1) {                                                      // while there are more files, store them
        struct dirent *de = readdir(dir);                            // read the next directory entry

        if(de == NULL) {                                            // no more entries?
            break;
        }

        if(de->d_type != DT_LNK) {                                    // if it's not a link, skip it
            continue;
        }

        char *pJoystick = strstr(de->d_name, "joystick");           // does the name contain 'joystick'?
        char *pEvent    = strstr(de->d_name, "event");              // does the name contain 'event'?

        if(pJoystick == NULL && pEvent == NULL) {                   // if it's not joystick and doesn't contain 'event', skip it (non-joystick must have event)
            continue;
        }

        if(pJoystick != NULL && pEvent != NULL) {                   // if it's joystick and contains 'event', skip it (joystick must NOT have event)
            continue;
        }

        memset(linkBuf,    0, PATH_BUFF_SIZE);
        memset(devBuf,    0, PATH_BUFF_SIZE);

        strcpy(linkBuf, INPUT_LINKS_PATH);                          // create path to link files, e.g. /dev/input/by-path/usb-kbd-event
        strcat(linkBuf, HOSTPATH_SEPAR_STRING);
        strcat(linkBuf, de->d_name);

        int ires = readlink(linkBuf, devBuf, PATH_BUFF_SIZE);        // try to resolve the filename from the link
        if(ires == -1) {
            continue;
        }

        std::string pathAndFile = devBuf;
        std::string path, file;

        Utils::splitFilenameFromPath(pathAndFile, path, file);        // get only file name, skip the path (which now is something like '../../')
        file = "/dev/input/" + file;                                // create full path - /dev/input/event0

        processFoundDev(de->d_name, file.c_str());    // and do something with that file
    }

    closedir(dir);
}

void Ikbd::findVirtualDevices(void)
{
    char devpath[PATH_BUFF_SIZE];

    std::string vdevFolder = Utils::dotEnvValue("IKBD_VIRTUAL_DEVICES_PATH");
    DIR *dir = opendir(vdevFolder.c_str());     // try to open the dir

    if(dir == NULL) {                                               // not found?
        return;
    }

    while(1) {                                                      // while there are more files, store them
        struct dirent *de = readdir(dir);                           // read the next directory entry

        if(de == NULL) {                                            // no more entries?
            break;
        }

        if(de->d_type != DT_FIFO) {                                    // if it's not a fifo, skip it
            continue;
        }

        char *pMouse  = strstr(de->d_name, "mouse");            // does the name contain 'mouse'?
        char *pKbd    = strstr(de->d_name, "kbd");              // does the name contain 'kbd'?

        if(pMouse == NULL && pKbd == NULL) {
            continue;
        }

        snprintf(devpath, sizeof(devpath), "%s/%s", vdevFolder.c_str(), de->d_name);

        processFoundDev(devpath, devpath);    // and do something with that file
    }

    closedir(dir);
}

void Ikbd::processFoundDev(const char *linkName, const char *fullPath)
{
    TInputDevice *in = NULL;
    const char *what;
    bool virtualDevice = false;

    std::string vdevMouse = Utils::dotEnvValue("IKBD_VIRTUAL_MOUSE_FILE");
    std::string vdevKbd = Utils::dotEnvValue("IKBD_VIRTUAL_KEYBOARD_FILE");

    logIkbd(LOG_INFO, "Ikbd::processFoundDev(%s, %s)", linkName, fullPath);
    if(vdevMouse == linkName) {                         // it's a VIRTUAL mouse
        if(ikbdDevs[INTYPE_VDEVMOUSE].fd == -1) {       // don't have mouse?
            in = &ikbdDevs[INTYPE_VDEVMOUSE];
            what = linkName;
            virtualDevice = true;
        } else {                                        // already have a mouse?
            logIkbd(LOG_INFO, "%s: already have a mouse", fullPath);
            return;
        }
    } else if(vdevKbd == linkName) {                    // it's a VIRTUAL keyboard?
        if(ikbdDevs[INTYPE_VDEVKEYBOARD].fd == -1) {          // don't have keyboard?
            in = &ikbdDevs[INTYPE_VDEVKEYBOARD];
            what = linkName;
            virtualDevice = true;
        } else {                                        // already have a keyboard?
            logIkbd(LOG_INFO, "%s: already have a keyboard", fullPath);
            return;
        }
    } else if(strstr(linkName, "mouse") != NULL) {             // it's a mouse
        if(ikbdDevs[INTYPE_MOUSE].fd == -1) {             // don't have mouse?
            in = &ikbdDevs[INTYPE_MOUSE];
            what = "mouse";
        } else {                                        // already have a mouse?
            logIkbd(LOG_INFO, "%s: already have a mouse", fullPath);
            return;
        }
    } else if(strstr(linkName, "kbd") != NULL) {               // it's a keyboard?
        if(ikbdDevs[INTYPE_KEYBOARD].fd == -1) {          // don't have keyboard?
            in = &ikbdDevs[INTYPE_KEYBOARD];
            what = "keyboard";
        } else {                                        // already have a keyboard?
            logIkbd(LOG_INFO, "%s: already have a keyboard", fullPath);
            return;
        }
    } else if(strstr(linkName, "joystick") != NULL) {                  // it's a joystick?
        // fixed slots: 1st stick -> joystick[1] (port 1 by default), 2nd stick -> joystick[0] (port 0 by default);
        // swapping ports (firstJoyIs0) is done when sending, see joyPort(), so it also applies to already connected sticks
        const int joy1st = INTYPE_JOYSTICK2;
        const int joy2nd = INTYPE_JOYSTICK1;

        if(ikbdDevs[joy1st].fd == -1) {                                       // don't have joystick 1?
            if(strcmp(fullPath, ikbdDevs[joy2nd].devPath) == 0) {             // if this device is already connected as joystick 2, skip it
                logIkbd(LOG_INFO, "%s: already connected as joystick 2", fullPath);
                return;
            }

            in = &ikbdDevs[joy1st];
            what = "joystick1";
        } else if(ikbdDevs[joy2nd].fd == -1) {                                // don't have joystick 2?
            if(strcmp(fullPath, ikbdDevs[joy1st].devPath) == 0) {             // if this device is already connected as joystick 1, skip it
                logIkbd(LOG_INFO, "%s: already cannected as joystick 1", fullPath);
                return;
            }

            in = &ikbdDevs[joy2nd];
            what = "joystick2";
        } else {                                                            // already have a joystick?
            logIkbd(LOG_INFO, "%s: already have both joysticks", fullPath);
            return;
        }
    } else {
        // this isn't mouse, keyboard or joystick, quit!
        return;
    }

    if(strlen(fullPath) >= sizeof(in->devPath)) {      // path wouldn't fit to devPath (virtual device path can be long)
        logIkbd(LOG_ERROR, "Input device path too long (max %d chars): %s", (int) sizeof(in->devPath) - 1, fullPath);
        return;
    }

    int fd = open(fullPath, O_RDONLY | O_NONBLOCK);

    if(fd < 0) {
        logIkbd(LOG_ERROR, "Failed to open input device %s (%s) : %s ", fullPath, what, strerror(errno));
        return;
    }

    // need to EVIOCSREP ?

    in->fd = fd;
    in->partialLen = 0;
    snprintf(in->devPath, sizeof(in->devPath), "%s", fullPath);
    logIkbd(LOG_INFO, "Got device (%s): %s", what, fullPath);

    // grab only real keyboard / mouse (evdev) - joysticks are /dev/input/js* where EVIOCGRAB doesn't work, virtual devices are FIFOs
    bool isKeybOrMouse = (in == &ikbdDevs[INTYPE_KEYBOARD] || in == &ikbdDevs[INTYPE_MOUSE]);
    if(!virtualDevice && isKeybOrMouse && keyboardExclusiveAccess) {
        grabExclusiveAccess(fd);
    }
}

void Ikbd::processMouse(input_event *ev)
{
    if(ev->type == EV_KEY) {        // on button press
        int btnNew = mouseBtnNow;

        // Following block checks if it's not a mouse button, and if it's not, it will let the keyboard code
        // handle this EV_KEY event. This is because Logitech K400 on RPi appears only as mouse device instead of mouse device + keyboard device,
        // but the EV_KEY events are still sent - through that single mouse device.
        if(ev->code != BTN_LEFT && ev->code != BTN_RIGHT) {
            // TODO: fix this later
            processKeyboard(ev, false);     // false: don't skip keyboard translation
            return;
        }

        uint8_t absButtons = 0;

        switch(ev->code) {
            case BTN_LEFT:
            if(ev->value == 1) {                // on DOWN - add bit
                btnNew |= 2;
                absButtons            |= MOUSEABS_BTN_LEFT_DOWN;
                absMouse.buttons    |= MOUSEABS_BTN_LEFT_DOWN;
            } else {                            // on UP - remove bit
                btnNew &= ~2;
                absButtons            |= MOUSEABS_BTN_LEFT_UP;
                absMouse.buttons    |= MOUSEABS_BTN_LEFT_UP;
            }
            break;

            case BTN_RIGHT:
            if(ev->value == 1) {                // on DOWN - add bit
                btnNew |= 1;
                absButtons            |= MOUSEABS_BTN_RIGHT_DOWN;
                absMouse.buttons    |= MOUSEABS_BTN_RIGHT_DOWN;
            } else {                            // on UP - remove bit
                btnNew &= ~1;
                absButtons            |= MOUSEABS_BTN_RIGHT_UP;
                absMouse.buttons    |= MOUSEABS_BTN_RIGHT_UP;
            }
            break;
        }

        if(btnNew == mouseBtnNow) {                                // mouse buttons didn't change?
            return;
        }

        mouseBtnNow = btnNew;                                    // store new button states

        // in keycode mouse mode (or when asked to) buttons are reported as keys 0x74 (left) / 0x75 (right)
        if(mouseMode == MOUSEMODE_KEYCODE || (mouseAbsBtnAct & MOUSEBTN_REPORT_ACTLIKEKEYS)) {
            uint8_t bfr;

            switch(absButtons) {
                case MOUSEABS_BTN_LEFT_DOWN:    bfr = 0x74; sendToSt(&bfr, 1); break;
                case MOUSEABS_BTN_LEFT_UP:      bfr = 0xf4; sendToSt(&bfr, 1); break;
                case MOUSEABS_BTN_RIGHT_DOWN:   bfr = 0x75; sendToSt(&bfr, 1); break;
                case MOUSEABS_BTN_RIGHT_UP:     bfr = 0xf5; sendToSt(&bfr, 1); break;
            }
        }

        if(mouseMode == MOUSEMODE_ABS) {                        // for absolute mouse mode
            bool wasUp = (absButtons & MOUSEABS_BTN_UP)            != 0;
            bool reportUp = (mouseAbsBtnAct & MOUSEBTN_REPORT_RELEASE)  != 0;
            bool wasDown = (absButtons & MOUSEABS_BTN_DOWN)        != 0;
            bool reportDown = (mouseAbsBtnAct    & MOUSEBTN_REPORT_PRESS)    != 0;

            if((wasUp && reportUp) || (wasDown && reportDown) ) {    // if button pressed / released and we should report that
                sendMousePosAbsolute(absButtons);
            }
        } else if(mouseMode == MOUSEMODE_REL) {                 // for relative mouse mode
            sendRelMousePackets(relMouse.accX, relMouse.accY);  // button change, together with not yet reported movement
            relMouse.accX = 0;
            relMouse.accY = 0;
        }
        return;
    }

    if(ev->type == EV_REL) {
        if(ev->code == REL_X) {
            mouseMoved(ev->value, 0);       // update absolute position, report movement as current mouse mode needs
        }

        if(ev->code == REL_Y) {
            mouseMoved(0, ev->value);
        }

        //--------------------------
        // mouse wheel moved and mouse wheel should act as UP / DOWN keys?
        if(ev->code == REL_WHEEL && mouseWheelAsArrowsUpDown) {
            int stKey;

            if(ev->value > 0) {
                stKey = keyTranslator.pcKeyToSt(KEY_UP);
            } else {
                stKey = keyTranslator.pcKeyToSt(KEY_DOWN);
            }

            if(stKey == 0) {    // key not found? quit
                return;
            }

            logIkbd(LOG_DEBUG, "\nEV_REL -> REL_WHEEL: ev->value: %d, stKey: %d", ev->value, stKey);

            uint8_t bfr[2];
            bfr[0] = stKey;                     // key down
            bfr[1] = stKey | 0x80;              // key up

            sendToSt(bfr, 2);
        }
    }
}

void Ikbd::processKeyboard(input_event *ev, bool skipKeyboardTranslation)
{
    int stKey = 0;
//    logIkbd(LOG_DEBUG, "processKeyboard");

    switch(ev->type) {
    case EV_SYN:
        //logIkbd("Ikbd::processKeyboard() EV_SYN code=%04x\n", ev->code);
        break;
    case EV_KEY:
        //logIkbd("Ikbd::processKeyboard() EV_KEY code=%04x ev->value=%d\n", ev->code, ev->value);

        if(ev->code >= KBD_KEY_COUNT) {
            return;
        }

        stKey = keyTranslator.pcKeyToSt(ev->code);          // translate PC key to ST key
        // ev->value -- 1: down, 2: auto repeat, 0: up
        if(ev->value == 0) {        // when key is released, ST scan code has the highest bit set
            stKey = stKey | 0x80;
            pressedKeys[ev->code] = false;
        } else if (ev->value == 1) {
            pressedKeys[ev->code] = true;
            if(pressedKeys[KEY_LEFTCTRL] && pressedKeys[KEY_RIGHTCTRL] && pressedKeys[KEY_LEFTALT] && pressedKeys[KEY_RIGHTALT]) {
                toggleKeyboardExclusiveAccess();
            }
        } else if (ev->value == 2) {
            // auto repeat has no place on Atari! :)
            return;
        }

        if(stKey == 0 || stKey == 0x80) {           // key not found? quit
            return;
        }

        skipKeyboardTranslation = handleHotkeys(ev->code, ev->value == 1, skipKeyboardTranslation);
        if (!skipKeyboardTranslation) {
            if(keybJoy0 && keyJoyKeys.isKeybJoyKeyPc(0, ev->code)) {    // Keyb joy 0 is enabled, and it's a keyb joy 0 key? Handle it specially.
                handlePcKeyAsKeybJoy(0, ev->code, ev->value);
                return;
            }

            if(keybJoy1 && keyJoyKeys.isKeybJoyKeyPc(1, ev->code)) {    // Keyb joy 1 is enabled, and it's a keyb joy 1 key? Handle it specially.
                handlePcKeyAsKeybJoy(1, ev->code, ev->value);
                return;
            }
        }

        logIkbd(LOG_DEBUG, "\nEV_KEY: code %d, value %d, stKey: %02x", ev->code, ev->value, stKey);

        uint8_t bfr;
        bfr = stKey;

        sendToSt(&bfr, 1);
        break;
    case EV_MSC:
        //logIkbd("Ikbd::processKeyboard() EV_MSC code=%d value=0x%08x\n", ev->code, ev->value);
        break;
    default:
        logIkbd(LOG_DEBUG, "Ikbd::processKeyboard() ***UNKNOWN*** ev->type=%d ev->code=0x%04x ev->value=0x%08x\n", ev->type, ev->code, ev->value);
    }
}

void Ikbd::processJoystick(js_event *jse, int joyNumber)
{
    TJoystickState *js;

    if(joyNumber == 0 || joyNumber == 1) {          // if the index is OK, use it
        js = &joystick[joyNumber];
    } else {                                        // index is not OK, quit
        return;
    }

    if(jse->type == JS_EVENT_AXIS) {
        if(jse->number >= JOYAXIS) {                // if the index of axis would be out of index, quit
            return;
        }

        js->axis[ jse->number ] = jse->value;       // store new axis value
    } else if(jse->type == JS_EVENT_BUTTON) {
        if(jse->number >= JOYBUTTONS) {             // if the index of button would be out of index, quit
            return;
        }

        js->button[ jse->number ] = jse->value;     // 1 means down, 0 means up
    } else {
        return;
    }

    int dirTotal, dirX, dirY;

    if(js->axis[0] < -16000) {          // joy left?
        dirX    = JOYDIR_LEFT;
    } else if(js->axis[0] > 16000) {    // joy right?
        dirX    = JOYDIR_RIGHT;
    } else {                            // joy in center
        dirX    = 0;
    }

    if(js->axis[1] < -16000) {          // joy up?
        dirY    = JOYDIR_UP;
    } else if(js->axis[1] > 16000) {    // joy down?
        dirY    = JOYDIR_DOWN;
    } else {                            // joy in center
        dirY    = 0;
    }

    dirTotal = dirX | dirY;             // merge left-right with up-down

    int button = 0;
    for(int i=0; i<JOYBUTTONS; i++) {   // check if any button is pressed
        if(js->button[i] != 0) {        // this button pressed? mark that something is pressed
            button = JOYDIR_BUTTON;
        }
    }

    // port 0: mouse + joystick
    // port 1: joystick

    bool dirChanged = (dirTotal != js->lastDir);            // dir changed flag
    bool btnChanged = (button != js->lastBtn);              // btn changed flag

    if(dirChanged || btnChanged) {                          // direction or button changed?
        js->lastDir = dirTotal;                             // always keep the state - interrogation reports it later
        js->lastBtn = button;

        if(joystickState == Disabled) {                     // ST disabled joysticks? don't report anything
            return;
        }

        if(joystickState == EnabledInMouseMode && btnChanged) {  // if button state changed, send it as mouse packet
            sendJoyButtonsInMouseMode();
            return;
        }

        if(joystickMode == JOYMODE_EVENT) {                 // only in event reporting mode the IKBD sends packets on its own
            sendJoyState(joyNumber, button | dirTotal);     // report current direction and buttons
        }

        // JOYMODE_INTERROGATION: ST asks, answered from lastDir / lastBtn (sendBothJoyReport)
        // JOYMODE_MONITORING, _FIRE_MONITORING, _KEYCODE: not supported - sending event packets would be wrong there
    }
}

int Ikbd::getFdByIndex(int index)
{
    if(index < 0 || index >= INTYPE_COUNT) {
        return -1;
    }

    return ikbdDevs[index].fd;
}

bool Ikbd::gotUsbMouse(void)
{
    if(ikbdDevs[INTYPE_MOUSE].fd != -1 ) {            // got real USB mouse? return true
        return true;
    }

    if(ikbdDevs[INTYPE_VDEVMOUSE].fd != -1) {                           // got virtual mouse?
        uint32_t diff = Utils::getCurrentMs() - lastVDevMouseEventTime;    // calculate how much time has passed since last VDevMouse event

        if(diff < 10000) {                                              // if virtual mouse moved in the last 10 seconds, we got it (otherwise we don't have it)
            return true;
        }
    }

    return false;                                   // no mouse present
}

bool Ikbd::gotUsbKeyboard(void)
{
    if(ikbdDevs[INTYPE_KEYBOARD].fd != -1) {
        return true;
    }

    return false;
}

bool Ikbd::gotUsbJoy1(void)
{
    if(ikbdDevs[INTYPE_JOYSTICK1].fd != -1) {
        return true;
    }

    return false;
}

bool Ikbd::gotUsbJoy2(void)
{
    if(ikbdDevs[INTYPE_JOYSTICK2].fd != -1) {
        return true;
    }

    return false;
}

void Ikbd::markVirtualMouseEvenTime(void)
{
    // store time when we received VDevMouse event -- for telling that
    // the virtual mouse is not here when it doesn't move for a longer time
    lastVDevMouseEventTime = Utils::getCurrentMs();
}

void Ikbd::handlePcKeyAsKeybJoy(int joyNumber, int pcKey, int eventValue)
{
    bool keyDown = (eventValue != 0);   // if eventValue is 0, then it's up; otherwise down
    handleKeyAsKeybJoy(true, joyNumber, pcKey, keyDown);  // handle this key press, and it comes from PC keys (therefore first param: true)
}

void Ikbd::handleKeyAsKeybJoy(bool pcNotSt, int joyNumber, int pcKey, bool keyDown)
{
    if(joyNumber < 0 || joyNumber > 1) {
        return;
    }

    bool *held = keybJoyHeld[joyNumber];
    js_event jse;
    memset(&jse, 0, sizeof(jse));

    // For each axis: the key just pressed sets the direction. On release, if the opposite key is still held,
    // the axis goes to that direction (not to center) - e.g. release UP while DOWN is held = DOWN.
    if(keyJoyKeys.isKeyUp(pcNotSt, joyNumber, pcKey) || keyJoyKeys.isKeyDown(pcNotSt, joyNumber, pcKey)) {
        bool up = keyJoyKeys.isKeyUp(pcNotSt, joyNumber, pcKey);
        held[up ? KJ_UP : KJ_DOWN] = keyDown;

        jse.type    = JS_EVENT_AXIS;        // axis movement
        jse.number  = 1;                    // it's Y axis
        if(keyDown) {
            jse.value = up ? -20000 : 20000;
        } else {
            jse.value = held[KJ_UP] ? -20000 : (held[KJ_DOWN] ? 20000 : 0);
        }
    } else if(keyJoyKeys.isKeyLeft(pcNotSt, joyNumber, pcKey) || keyJoyKeys.isKeyRight(pcNotSt, joyNumber, pcKey)) {
        bool left = keyJoyKeys.isKeyLeft(pcNotSt, joyNumber, pcKey);
        held[left ? KJ_LEFT : KJ_RIGHT] = keyDown;

        jse.type    = JS_EVENT_AXIS;        // axis movement
        jse.number  = 0;                    // it's X axis
        if(keyDown) {
            jse.value = left ? -20000 : 20000;
        } else {
            jse.value = held[KJ_LEFT] ? -20000 : (held[KJ_RIGHT] ? 20000 : 0);
        }
    } else if(keyJoyKeys.isKeyButton(pcNotSt, joyNumber, pcKey)) {
        held[KJ_BUTTON] = keyDown;

        jse.type    = JS_EVENT_BUTTON;      // button press
        jse.number  = 0;                    // it's button 0 (fire)
        jse.value   = keyDown ? 1 : 0;      // keyboard key down? 1 means down, 0 means up
    } else {
        return;                             // not a key of this keyboard joystick
    }

    processJoystick(&jse, joyNumber);
}

void Ikbd::toggleKeyboardExclusiveAccess(void)
{
    int fdKeyb, fdMouse;

    fdKeyb  = getFdByIndex(INTYPE_KEYBOARD);
    fdMouse = getFdByIndex(INTYPE_MOUSE);

    logIkbd(LOG_DEBUG, "Ikbd::toggleKeyboardExclusiveAccess() fdKeyb=%d, fdMouse=%d, keyboardExclusiveAccess=%d\n", fdKeyb, fdMouse, keyboardExclusiveAccess);

    if(noCapture) {                         // capturing disabled from command line - nothing to toggle
        logIkbd(LOG_DEBUG, "Ikbd::toggleKeyboardExclusiveAccess() - capture disabled (noCapture), not grabbing");
        return;
    }

    keyboardExclusiveAccess = !keyboardExclusiveAccess;     // capture mode - also applies to keyboard / mouse found later

    if(keyboardExclusiveAccess) {
        grabExclusiveAccess(fdKeyb);
        grabExclusiveAccess(fdMouse);
    } else {
        releaseExclusiveAccess(fdKeyb);
        releaseExclusiveAccess(fdMouse);
    }
}

bool Ikbd::grabExclusiveAccess(int fd)
{
    if(noCapture) {       // if shouldn't do USB keyboard and mouse capture, just quit
        return false;
    }

    if(fd < 0) {          // not open (0 is a valid fd)
        return false;
    }

    if(ioctl(fd, EVIOCGRAB, (void*)1) < 0) { // grab exclusive
        logIkbd(LOG_ERROR, "Ikbd::grabExclusiveAccess() ioctl failed : %s", strerror(errno));
        return false;
    }

    return true;
}

void Ikbd::releaseExclusiveAccess(int fd)
{
    if(fd < 0) {          // not open (0 is a valid fd)
        return;
    }

    if(ioctl(fd, EVIOCGRAB, (void*)0) < 0) { // release
        logIkbd(LOG_ERROR, "Ikbd::releaseExclusiveAccess() ioctl failed : %s", strerror(errno));
    }
}

bool Ikbd::handleHotkeys(int pcKey, bool pressed, bool skipKeyboardTranslation)
{
    logIkbd(LOG_DEBUG, "Ikbd::handleHotkeys - %d, %d, %d", pcKey, pressed, skipKeyboardTranslation);

    if (pcKey == KEY_LEFTCTRL || pcKey == KEY_RIGHTCTRL) {
        if (!pressed) {
            if (ctrlsPressed > 0) {     // never below 0 - a release without seen press (e.g. key held before device was found) must not break hotkeys
                ctrlsPressed--;
            }
        } else {
            ctrlsPressed++;
        }
    }

    if (pcKey == KEY_LEFTSHIFT) {
        if (!pressed) {
            if (leftShiftsPressed > 0) {     // never below 0 - a release without seen press (e.g. key held before device was found) must not break hotkeys
                leftShiftsPressed--;
            }
        } else {
            leftShiftsPressed++;
        }
    }

    if (pcKey == KEY_RIGHTSHIFT) {
        if (!pressed) {
            if (rightShiftsPressed > 0) {     // never below 0 - a release without seen press (e.g. key held before device was found) must not break hotkeys
                rightShiftsPressed--;
            }
        } else {
            rightShiftsPressed++;
        }
    }

    if (pcKey == KEY_F11) {
        if (!pressed) {
            if (f11sPressed > 0) {     // never below 0 - a release without seen press (e.g. key held before device was found) must not break hotkeys
                f11sPressed--;
            }
        } else {
            f11sPressed++;
        }
    }

    if (pcKey == KEY_F12) {
        if (!pressed) {
            if (f12sPressed > 0) {     // never below 0 - a release without seen press (e.g. key held before device was found) must not break hotkeys
                f12sPressed--;
            }
        } else {
            f12sPressed++;
        }
    }

    if (skipKeyboardTranslation) {
        return true;
    }

    if ((leftShiftsPressed > 0 || rightShiftsPressed > 0) && ctrlsPressed > 0 && pressed) {
        Settings s;
        if (f11sPressed > 0) {
            firstJoyIs0 = !firstJoyIs0;
            s.setBool("JOY_FIRST_IS_0", firstJoyIs0);
            sendAllJoyStates();             // ports swapped - tell ST the current state of both ports, so no direction stays stuck on the old port
            logIkbd(LOG_DEBUG, "Ikbd::handleHotkeys - joy0 <-> joy1 [before: %s, now: %s]",
                     firstJoyIs0 ? "OFF" : "ON", firstJoyIs0 ? "ON" : "OFF");
        } else if (f12sPressed > 0) {
            if (leftShiftsPressed > 0) {
                keybJoy0 = !keybJoy0;
                s.setBool("KEYBOARD_JOY0", keybJoy0);
                logIkbd(LOG_DEBUG, "Ikbd::handleHotkeys - toggle joy0 translation [before: %s, now: %s]",
                         keybJoy0 ? "OFF" : "ON", keybJoy0 ? "ON" : "OFF");
                waitingForHotkeyRelease = true;
            }
            if (rightShiftsPressed > 0) {
                keybJoy1 = !keybJoy1;
                s.setBool("KEYBOARD_JOY1", keybJoy1);
                logIkbd(LOG_DEBUG, "Ikbd::handleHotkeys - toggle joy1 translation [before: %s, now: %s]",
                         keybJoy1 ? "OFF" : "ON", keybJoy1 ? "ON" : "OFF");
                waitingForHotkeyRelease = true;
            }
        }
    }

    if (waitingForHotkeyRelease) {
        bool oldWaitingForHotkeyRelease = waitingForHotkeyRelease;
        waitingForHotkeyRelease =
                leftShiftsPressed > 0 || rightShiftsPressed > 0 || ctrlsPressed > 0
                || f11sPressed > 0 || f12sPressed > 0;
        if (oldWaitingForHotkeyRelease == waitingForHotkeyRelease) {
            logIkbd(LOG_DEBUG, "Ikbd::handleHotkeys - hotkey release pending...");
        } else {
            logIkbd(LOG_DEBUG, "Ikbd::handleHotkeys - hotkey release not pending anymore");
        }
        return true;
    } else {
        return false;
    }
}
