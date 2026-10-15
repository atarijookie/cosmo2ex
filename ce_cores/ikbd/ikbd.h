// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef _IKBD_H_
#define _IKBD_H_

#include <termios.h>
#include <linux/joystick.h>

#include <bitset>

#include "ikbd_defs.h"
#include "cyclicbuff.h"
#include "../chipinterface/chipinterfacedefs.h"     // MAX_CLIENTS
#include "keybjoys.h"
#include "keytranslator.h"

//#define SPYIKBD

#define INPUT_LINKS_PATH        "/dev/input/by-path"
#define HOSTPATH_SEPAR_STRING   "/"
#define PATH_BUFF_SIZE            1024

#define INPUT_EVENT_MAX_SIZE    32          // >= sizeof(input_event) and sizeof(js_event)

typedef struct {
    char    devPath[256];
    int     fd;

    uint8_t partial[INPUT_EVENT_MAX_SIZE];  // part of event received so far - a read can return less than whole event (e.g. from FIFO)
    int     partialLen;
} TInputDevice;

#define JOYAXIS             8
#define JOYBUTTONS          12

// count of key to check for key combination
#define KBD_KEY_COUNT   256

#define UARTMARK_STCMD      0xAA
#define UARTMARK_KEYBDATA   0xBB
#define UARTMARK_ALIVE      0xEE

typedef struct {
    int axis[JOYAXIS];
    int button[JOYBUTTONS];

    int lastDir;
    int lastBtn;

    int lastDirAndBtn;
} TJoystickState;

void *ikbdThreadCode(void *ptr);
class ChipInterface;

// state which belongs to one connected device (one ST) - its data stream and its link to the ST
struct IkbdClientState {
    CyclicBuff  receivedData;           // raw tagged data from device
    CyclicBuff  stCommands;             // ST -> IKBD command bytes
    CyclicBuff  keyboardData;           // original keyboard -> ST bytes
    CyclicBuff  pausedOutput;           // data for ST queued while ST paused the output

    int         ceIkbdMode;             // SOLO, or INJECT when this ST has its own (original) keyboard sending data
    bool        outputEnabled;          // false after ST sent PAUSE OUTPUT, until any other command

    void reset(void) {
        receivedData.init();
        stCommands.init();
        keyboardData.init();
        pausedOutput.init();
        ceIkbdMode      = CE_IKBDMODE_SOLO;
        outputEnabled   = true;
    }
};

extern TInputDevice ikbdDevs[INTYPE_COUNT];

class Ikbd
{
public:
    Ikbd(ChipInterface* ciIkbd);

    void loadSettings(void);

    void findDevices(void);
    void findVirtualDevices();
    void closeDevs(void);

    int getFdByIndex(int index);
    void deinitDev(int index);

    void processMouse(input_event *ev);
    void processKeyboard(input_event *ev, bool skipKeyboardTranslation);
    void processJoystick(js_event *jse, int joyNumber);
    void markVirtualMouseEvenTime(void);

    int processReceivedCommands(bool skipKeyboardTranslation, int fdUartRead);     // returns bytes read, 0 = nothing now, -1 = connection closed / broken

    void clientConnected(int clientIndex);      // new device in this client slot - start with clean state

private:
    ChipInterface* ciIkbd;

    enum JoystickState {
        Disabled,
        EnabledInMouseMode, // joy1 direction works but its button is mapped as mouse; joy0 is treated as mouse
        Enabled // joy1 and joy0 are treated as joysticks
    };

    IkbdClientState clientState[MAX_CLIENTS];  // per device (ST) state
    IkbdClientState noClientState;              // used when not processing data of any client
    IkbdClientState *cur;                       // state of the client whose data is being processed now
    int             curClientIndex;             // index of that client, -1 = none (output then goes to all clients)

    int     mouseBtnNow;
    int     mouseMode;
    bool    mouseEnabled;
    bool    mouseY0atTop;
    int     mouseAbsBtnAct;

    int     joy1st;
    int     joy2nd;

    bool    firstJoyIs0;                    // if true, joystick keyboard mapping and usb/atari joysticks are swapped
    bool    mouseWheelAsArrowsUpDown;       // if true, mouse wheel up / down will be translated to arrow up / down
    bool    keybJoy0;                       // if true, specific keys will act as joy 0
    bool    keybJoy1;                       // if true, specific keys will act as joy 1

    // this is basically all keys which are used for the hotswap, perhaps
    // it should be made a bit more flexible, for different key combinations
    int     leftShiftsPressed;  // how many LEFT SHIFTs are pressed (1x for PC, 1x for Atari)
    int     rightShiftsPressed; // how many RIGHT SHIFTs are pressed (1x for PC, 1x for Atari)
    int     ctrlsPressed;       // how many C[ON]TR[O]Ls are pressed (2x for PC, 1x for Atari)
    int     f11sPressed;        // HELP on Atari, F11 on PC
    int     f12sPressed;        // UNDO on Atari, F12 on PC
    bool    waitingForHotkeyRelease;

    KeybJoyKeys     keyJoyKeys;
    KeyTranslator   keyTranslator;

    struct {
        int        threshX, threshY;
        int        accX, accY;          // movement not reported yet (below threshold)
    } relMouse;

    struct {
        int        x,y;
        int        maxX, maxY;
        int        scaleX, scaleY;
        int        fracX, fracY;        // movement not turned into position yet (below scale)
        uint8_t    buttons;
    } absMouse;

    struct {
        uint8_t deltaX;
        uint8_t deltaY;
        int     accX, accY;             // movement not turned into cursor keys yet
    } keycodeMouse;

    uint32_t           lastVDevMouseEventTime;

    int             joystickMode;
    TJoystickState  joystick[2];
    JoystickState   joystickState;


    uint8_t            specialCodeLen[10];
    uint8_t            stCommandLen[256];

    std::bitset<KBD_KEY_COUNT> pressedKeys;
    bool            keyboardExclusiveAccess;    // true when keyboard / mouse are grabbed (other programs don't get their events)

    // keys of keyboard joysticks which are held now: [joy][KJ_UP .. KJ_BUTTON]
    enum { KJ_UP, KJ_DOWN, KJ_LEFT, KJ_RIGHT, KJ_BUTTON, KJ_COUNT };
    bool            keybJoyHeld[2][KJ_COUNT];

    void initDevs(void);
    void initJoystickState(TJoystickState *joy);
    void fillKeyTranslationTable(void);
    void addToTable(int pcKey, int stKey, int humanKey=0);
    void fillSpecialCodeLengthTable(void);
    void fillStCommandsLengthTable(void);

    void processFoundDev(const char *linkName, const char *fullPath);

    void resetInternalIkbdVars(void);
    void sendJoyButtonsInMouseMode(void);
    void sendJoyState(int joyNumber, int dirTotal);
    void sendBothJoyReport(void);
    void mouseMoved(int dx, int dy);                    // USB mouse movement, dy > 0 = down
    void sendRelMousePackets(int dx, int dy);           // relative packet(s) with current buttons, split to fit -128..127
    uint8_t mouseButtonsWithJoy(void);                  // mouse buttons + joystick fire buttons acting as mouse buttons
    void sendKeyPress(uint8_t stKey);                   // make + break code
    void sendMousePosAbsolute(uint8_t absButtons);
    void fixAbsMousePos(void);

    void processStCommands(void);
    void processGetCommand(uint8_t getCmd);
    void processKeyboardData(bool skipKeyboardTranslation);

    bool gotUsbMouse(void);
    bool gotUsbKeyboard(void);
    bool gotUsbJoy1(void);
    bool gotUsbJoy2(void);

    void handlePcKeyAsKeybJoy(int joyNumber, int pcKey, int eventValue);
    bool handleStKeyAsKeybJoy(uint8_t val);
    void handleKeyAsKeybJoy  (bool pcNotSt, int joyNumber, int pcKey, bool keyDown);
    bool handleHotkeys(int pcKey, bool pressed, bool skipKeyboardTranslation);

    void toggleKeyboardExclusiveAccess(void);
    bool grabExclusiveAccess(int fd);       // true if grabbed
    void releaseExclusiveAccess(int fd);

    void dumpBuffer(bool fromStNotKeyboard);

    void resetHotkeyState(void);

    // all data for ST goes through here: to the client being processed, otherwise (USB input) to all clients
    void sendToSt(const uint8_t* bfr, int len);
    void sendToClient(int clientIndex, const uint8_t* bfr, int len);
};

#endif

