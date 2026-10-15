#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <vector>

#include "../misc/global.h"
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "ikbd.h"
#include "../chipinterface/chipinterface.h"

int Ikbd::processReceivedCommands(bool skipKeyboardTranslation, int fdUartRead)
{
    if(fdUartRead == -1) {                                      // uart not open? quit
        return 0;
    }

    //-----------------
    // receive if there is something to receive
    uint8_t bfr[128];
    int bytesRead = read(fdUartRead, bfr, 128);                       // try to read data from uart

    if(bytesRead == 0) {                                        // peer closed the connection - caller must close it, otherwise select() keeps reporting it readable
        return -1;
    }

    if(bytesRead < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {     // just nothing to read now
            return 0;
        }

        logIkbd(LOG_ERROR, "Ikbd::processReceivedCommands - read(%d) failed : %s", fdUartRead, strerror(errno));
        return -1;                                              // real error - connection is broken, caller must close it
    }

    ClientInfo* ci = ciIkbd->clientGetByFd(fdUartRead);   // which device is this? each has its own buffers and modes
    if(!ci || ci->index < 0 || ci->index >= MAX_CLIENTS) {
        logIkbd(LOG_ERROR, "Ikbd::processReceivedCommands - fd %d is not a known client", fdUartRead);
        return -1;
    }

    cur             = &clientState[ci->index];             // from now on work with this device's state,
    curClientIndex  = ci->index;                            // and send replies only to its ST

    // if some data arrived, split it into
    for(int i=0; i<bytesRead; i++) {                        // add all the received data into cyclic buffer (in case it comes in pieces)
        cur->receivedData.add(bfr[i]);
    }

    while(true) {                                           // divide the received data to ST commands, keyboard data and debug strings
        if(cur->receivedData.count < 2) {                      // not enough data to work on? quit this loop (the data comes as TAG_byte and VALUE_byte)
            break;
        }

        uint8_t tag = cur->receivedData.peek();                // peek current value, don't move just yet
        uint8_t val;

        if(tag != UARTMARK_STCMD && tag != UARTMARK_KEYBDATA && tag != UARTMARK_ALIVE) {    // not a valid tag?
            tag = cur->receivedData.get();                     // remove byte from buffer and try again
            continue;
        }

        if(tag == UARTMARK_STCMD || tag == UARTMARK_KEYBDATA) { // for ST command and keyboard data
            tag = cur->receivedData.get();                     // get and remove tag from buffer
            val = cur->receivedData.get();                     // get and remove value from buffer

            if(tag == UARTMARK_STCMD) {                     // this is ST command byte, add it to ST command buffer
                cur->stCommands.add(val);
            } else {                                        // this is original keyboard data, add it to keyboard data buffer
                cur->keyboardData.add(val);
            }

            continue;
        }

        // this is just alive sign from device, remove it twice from buffer (tag + val)
        if(tag == UARTMARK_ALIVE) {
            cur->receivedData.get();
            cur->receivedData.get();
            continue;
        }
    }

    //-----------------
    // send if there are enough data to be sent
    #ifndef SPYIKBD                                             // if not spying on IKBD, process this normaly
        if(cur->stCommands.count > 0) {                            // got ST commands? process them
            processStCommands();
        }

        if(cur->keyboardData.count > 0) {                          // got keyboard data? process them
            processKeyboardData(skipKeyboardTranslation);
        }
    #else                                                       // if spying on IKBD, just resend data
        if(cur->stCommands.count > 0) {                            // got ST commands? process them
            dumpBuffer(true);
        }

        if(cur->keyboardData.count > 0) {                          // got keyboard data? process them
            dumpBuffer(false);
        }
    #endif

    cur             = &noClientState;                       // done with this device - output from USB devices goes to all STs again
    curClientIndex  = -1;

    return bytesRead;
}

void Ikbd::dumpBuffer(bool fromStNotKeyboard)
{
    std::string text;
    CyclicBuff *cb;

    if(fromStNotKeyboard) {
        text = "ST says      : ";
        cb = &cur->stCommands;
    } else {
        text = "Keyboard says: ";
        cb = &cur->keyboardData;
    }

    char bfr[16];

    while(cb->count > 0) {                      // if there's some data
        uint8_t val = cb->get();                   // get data

        sprintf(bfr, "%02x ", val);             // add hex dump of this data
        text += bfr;

        if(!fromStNotKeyboard) {                // if it's from keyboard, send it to ST
            sendToSt(&val, 1);
        }
    }

    logIkbd(LOG_DEBUG, text.c_str());                      // show the hex dump of data
}

void Ikbd::processStCommands(void)
{
    uint8_t bfr[512];

    if(cur->stCommands.count <= 0) {                               // no data? quit
        return;
    }

    while(cur->stCommands.count > 0) {                             // while there are some data, process
        uint8_t cmd = cur->stCommands.peek();                         // get the data, but don't move the get pointer, because we might fail later
        int len = 0;

        logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- got command %02x", cmd);

        if(cmd == STCMD_RESET || (cmd & 0x80) == 0) {           // reset or highest bit is 0? SET command
            len = stCommandLen[cmd];                            // try to get the command length
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- got command %02x, len %d", cmd, len);
        } else {                                                // highest bit is 1? GET command
            uint8_t setEquivalent = cmd & 0x7f;                    // get the equivalent SET command (remove higherst bit)
            uint8_t setLen = stCommandLen[setEquivalent];          // try to get the length of SET equivalent command

            if(setLen != 0) {                                   // if we got the SET equivalent command length, we support this command
                len = 1;                                        // the length of command is 1
            }

            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- got command %02x (GET), len %d", cmd, len);
        }

        logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- got command %02x, len: %d, cb contains %d bytes", cmd, len, cur->stCommands.count);

        if(len == 0) {                                          // it's not GET command and we don't have this SET command defined?
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- not GET cmd, and we don't know what to do");
            cur->stCommands.get();                                 // just remove this byte and try the next byte
            continue;
        }

        if(cmd == STCMD_MEMORY_LOAD) {                          // special case: this command has length stored on index[3]
            logIkbd(LOG_DEBUG, "Ikbd::processStCommands -- ST says: STCMD_MEMORY_LOAD");

            if(cur->stCommands.count < 4) {                        // not enough data to determine command length? quit
                return;
            }

            // 0x20 ADRMSB ADRLSB NUM + NUM data bytes - the whole thing is one command, data bytes must not be parsed as commands
            len = STCMD_MEMORY_LOAD_LEN + cur->stCommands.peekWithOffset(3);
        }

        if(len > cur->stCommands.count) {                          // if we don't have enough data in the buffer, quit
            //logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- not enough data in buffer, quitting (%d > %d)", len, cur->stCommands.count);
            return;
        }

        // if we got here, it's either GET or SET command that we support

        for(int i=0; i<len; i++) {                              // get the whole sequence
            bfr[i] = cur->stCommands.get();
        }

        if(bfr[0] != STCMD_PAUSE_OUTPUT && !cur->outputEnabled) {   // any command other than PAUSE OUTPUT resumes the output
            cur->outputEnabled = true;

            std::vector<uint8_t> queued;                        // send what was queued while paused
            while(cur->pausedOutput.count > 0) {
                queued.push_back(cur->pausedOutput.get());
            }
            if(!queued.empty()) {
                sendToSt(queued.data(), (int) queued.size());
            }
        }

        if(cmd != STCMD_RESET && (cmd & 0x80) != 0) {			// is it a GET command?
			processGetCommand(cmd);
			continue;
		}

        switch(bfr[0]) {
            // other commands
            //--------------------------------------------
            case STCMD_RESET:                           // reset keyboard - set everything to default values
            if (bfr[1] != 0x01) {
                logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_RESET but not followed by 0x01, skipping");
                break;
            }

            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_RESET");

            resetInternalIkbdVars();
            // there's nothing more to do, the IKBD will send "0xF?" as reply and we just pass it forward
            break;

            //--------------------------------------------
            case STCMD_PAUSE_OUTPUT:                    // disable any output to IKBD
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_PAUSE_OUTPUT");

            cur->outputEnabled = false;
            break;

            //////////////////////////////////////////////
            // joystick related commands
            //--------------------------------------------
            case STCMD_SET_JOYSTICK_EVENT_REPORTING:    // joystick mode: event reporting
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_JOYSTICK_EVENT_REPORTING");

            joystickMode = JOYMODE_EVENT;
            joystickState = Enabled;
            break;

            //--------------------------------------------
            case STCMD_SET_JOYSTICK_INTERROG_MODE:      // joystick mode: interrogation mode
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_JOYSTICK_INTERROG_MODE");

            joystickMode = JOYMODE_INTERROGATION;
            joystickState = Enabled;
            break;

            //--------------------------------------------
            case STCMD_SET_JOYSTICK_MONITORING:      // joystick mode: monitoring
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_JOYSTICK_MONITORING");

            joystickMode = JOYMODE_MONITORING;
            joystickState = Enabled;
            break;

            //--------------------------------------------
            case STCMD_SET_FIRE_BUTTON_MONITORING:      // joystick mode: fire button monitoring
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_FIRE_BUTTON_MONITORING");

            joystickMode = JOYMODE_FIRE_MONITORING;
            joystickState = Enabled;
            break;

            //--------------------------------------------
            case STCMD_SET_JOYSTICK_KEYCODE_MODE:      // joystick mode: keycode
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_JOYSTICK_KEYCODE_MODE");

            joystickMode = JOYMODE_KEYCODE;
            joystickState = Enabled;
            break;

            //--------------------------------------------
            case STCMD_JOYSTICK_INTERROGATION:
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_JOYSTICK_INTERROGATION");

			if(cur->ceIkbdMode != CE_IKBDMODE_SOLO) {		// if we're not in solo mode, don't answer (will be handled when the data from keyboard arrive)
				break;
			}

			if(joystickMode != JOYMODE_INTERROGATION) {	// if we're not in joy interrogation mode, ignore
				break;
			}

			sendBothJoyReport();
			break;

            //--------------------------------------------
            case STCMD_DISABLE_JOYSTICKS:               // disable joystick; any valid joystick command enabled joystick
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_DISABLE_JOYSTICKS");

            joystickState = Disabled;
            break;

            //////////////////////////////////////////////
            // mouse related commands
            //--------------------------------------------
            case STCMD_SET_REL_MOUSE_POS_REPORTING:     // set relative mouse reporting
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_REL_MOUSE_POS_REPORTING");

            mouseMode = MOUSEMODE_REL;
            mouseEnabled = true;
            joystickState = EnabledInMouseMode;
            relMouse.accX = 0;                          // start reporting from scratch
            relMouse.accY = 0;
            break;

            //--------------------------------------------
			case STCMD_SET_MOUSE_KEYCODE_MODE:			// set keycode mouse reporting
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_MOUSE_KEYCODE_MODE");

            mouseMode = MOUSEMODE_KEYCODE;
            mouseEnabled = true;
            joystickState = EnabledInMouseMode;
            keycodeMouse.deltaX = bfr[1];               // mouse units per one cursor key press
            keycodeMouse.deltaY = bfr[2];
            keycodeMouse.accX = 0;
            keycodeMouse.accY = 0;
            break;

            //--------------------------------------------
			case STCMD_SET_MOUSE_THRESHOLD:
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_MOUSE_THRESHOLD");

			relMouse.threshX	= bfr[1];                // mouse units needed before a relative packet is sent
			relMouse.threshY	= bfr[2];
            break;

            //--------------------------------------------
			case STCMD_SET_MOUSE_SCALE:
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_MOUSE_SCALE");

			absMouse.scaleX		= bfr[1];                // mouse units per one absolute position step
			absMouse.scaleY		= bfr[2];
            absMouse.fracX      = 0;
            absMouse.fracY      = 0;
            break;

			//--------------------------------------------
            case STCMD_SET_ABS_MOUSE_POS_REPORTING:     // set absolute mouse reporting
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_ABS_MOUSE_POS_REPORTING");

            mouseMode = MOUSEMODE_ABS;

            // read max X and Y max values
            absMouse.maxX = (((uint16_t) bfr[1]) << 8) | ((uint16_t) bfr[2]);
            absMouse.maxY = (((uint16_t) bfr[3]) << 8) | ((uint16_t) bfr[4]);

            logIkbd(LOG_DEBUG,  "new mouse max: [%04x, %04x]", absMouse.maxX, absMouse.maxY);

            fixAbsMousePos();                           // if absolute coordinates are out of bounds, fix them

            mouseEnabled = true;
            joystickState = EnabledInMouseMode;
            break;

            //--------------------------------------------
			case STCMD_INTERROGATE_MOUSE_POS:			// get the current absolute mouse position
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_INTERROGATE_MOUSE_POS");

			if(mouseMode != MOUSEMODE_ABS) {			// this is only valid in absolute mouse mode
                logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- STCMD_INTERROGATE_MOUSE_POS -- not sending anything because not in ABS mouse mode");
				break;
			}

			if(cur->ceIkbdMode == CE_IKBDMODE_INJECT && !gotUsbMouse()) {	// when working in INJECT mode and don't have mouse, don't answer
                logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- STCMD_INTERROGATE_MOUSE_POS -- not sending anything because we're in INJECT mode and we don't have USB mouse");
				break;
			} else {
                logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- STCMD_INTERROGATE_MOUSE_POS -- will send USB mouse position (either we're in SOLO mode and / or we gotUsbMouse() )");
            }

			sendMousePosAbsolute(absMouse.buttons);				// send position and accumulated buttons
			absMouse.buttons = 0;										// no buttons for now
			break;

            //--------------------------------------------
            case STCMD_LOAD_MOUSE_POS:                  // load new absolute mouse position
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_LOAD_MOUSE_POS");

            // read max X and Y position values
            absMouse.x = (((uint16_t) bfr[2]) << 8) | ((uint16_t) bfr[3]);
            absMouse.y = (((uint16_t) bfr[4]) << 8) | ((uint16_t) bfr[5]);

            logIkbd(LOG_DEBUG,  "new mouse pos  : [%04x, %04x]", absMouse.x, absMouse.y);

            absMouse.fracX = 0;                         // position set exactly - no partial step left
            absMouse.fracY = 0;

            fixAbsMousePos();                           // if absolute coordinates are out of bounds, fix them
            break;

            //--------------------------------------------
            case STCMD_SET_MOUSE_BTN_ACTION:            // set position reporting for absolute mouse mode on mouse button press
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_MOUSE_BTN_ACTION - param: %02X", (int) bfr[1]);

            mouseAbsBtnAct = bfr[1];					// store flags what we should report
            break;

            //--------------------------------------------
            case STCMD_SET_Y_AT_TOP:                    // Y=0 at top
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_Y_AT_TOP");

            mouseY0atTop = true;
            break;

            //--------------------------------------------
            case STCMD_SET_Y_AT_BOTTOM:                 // Y=0 at bottom
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_SET_Y_AT_BOTTOM");

            mouseY0atTop = false;
            break;

            //--------------------------------------------
            case STCMD_DISABLE_MOUSE:                   // disable mouse; any valid mouse command enables mouse
            logIkbd(LOG_DEBUG,  "Ikbd::processStCommands -- ST says: STCMD_DISABLE_MOUSE");

            mouseEnabled = false;
            break;

        default: {
            char str[256] = "Ikbd::processStCommands -- ignoring ";
            char tmp[16];
            for (int i = 0; i < len; ++i) {
                sprintf(tmp, "%02x ", bfr[i]);
                strcat(str, tmp);
            }
            logIkbd(LOG_DEBUG, str);
        }
        }
    }
}

void Ikbd::processGetCommand(uint8_t getCmd)
{
	if(cur->ceIkbdMode != CE_IKBDMODE_SOLO) {							// not SOLO mode? don't reply!
		return;
	}

	uint8_t setEquivalent = getCmd & 0x7f;								// remove highest bit

	bool send = false;

	uint8_t bfr[8];
	memset(bfr, 0, 8);

	bfr[0] = KEYBDATA_STATUS;

	switch(setEquivalent) {
		case STCMD_SET_REL_MOUSE_POS_REPORTING:		// relative mouse: report mouse mode
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_REL_MOUSE_POS_REPORTING");

		bfr[1] = mouseMode;
		send = true;
		break;

		case STCMD_SET_ABS_MOUSE_POS_REPORTING:		// absolute mouse: report mode and maximums
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_ABS_MOUSE_POS_REPORTING");

		bfr[1] = mouseMode;
		bfr[2] = (uint8_t) (absMouse.maxX >> 8);
		bfr[3] = (uint8_t)  absMouse.maxX;
		bfr[4] = (uint8_t) (absMouse.maxY >> 8);
		bfr[5] = (uint8_t)  absMouse.maxY;
		send = true;
		break;

		case STCMD_SET_MOUSE_KEYCODE_MODE:			// keycode mouse: report mode and deltas
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_MOUSE_KEYCODE_MODE");

		bfr[1] = mouseMode;
		bfr[2] = keycodeMouse.deltaX;
		bfr[3] = keycodeMouse.deltaY;
		send = true;
		break;

		case STCMD_SET_Y_AT_TOP:					// report mouse Y=0 location (top or bottom)
		case STCMD_SET_Y_AT_BOTTOM:
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_Y_AT_TOP or STCMD_SET_Y_AT_BOTTOM");

		bfr[1] = mouseY0atTop ? STCMD_SET_Y_AT_TOP : STCMD_SET_Y_AT_BOTTOM;
		send = true;
		break;

		case STCMD_DISABLE_MOUSE:					// report if mouse is disabled
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_DISABLE_MOUSE");

		bfr[1] = mouseEnabled ? 0 : STCMD_DISABLE_MOUSE;
		send = true;
		break;

		case STCMD_SET_JOYSTICK_EVENT_REPORTING:	// report joystick mode
		case STCMD_SET_JOYSTICK_INTERROG_MODE:
		case STCMD_SET_JOYSTICK_KEYCODE_MODE:
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_JOYSTICK _EVENT_REPORTING or _INTERROG_MODE or _KEYCODE_MODE");

		bfr[1] = joystickMode;
		send = true;
		break;

		case STCMD_DISABLE_JOYSTICKS:				// report if joystick is disabled
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_DISABLE_JOYSTICK");

        bfr[1] = joystickState != Disabled ? 0 : STCMD_DISABLE_JOYSTICKS;
		send = true;
		break;

		case STCMD_SET_MOUSE_BTN_ACTION:			// report mouse button actions
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_MOUSE_BTN_ACTION");

		bfr[1] = mouseAbsBtnAct;
		send = true;
		break;

		case STCMD_SET_MOUSE_THRESHOLD:				// report current mouse threshold
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_MOUSE_THRESHOLD");

		bfr[1] = relMouse.threshX;
		bfr[2] = relMouse.threshY;
		send = true;
		break;

		case STCMD_SET_MOUSE_SCALE:					// report current mouse scale
        logIkbd(LOG_DEBUG,  "Ikbd::processGetCommand -- STCMD_SET_MOUSE_SCALE");

		bfr[1] = absMouse.scaleX;
		bfr[2] = absMouse.scaleY;
		send = true;
		break;
	}

	if(send) {										// if command was handled
		sendToSt(bfr, 8);
	}
}

void Ikbd::fixAbsMousePos(void)
{
    if(absMouse.x < 0) {                                            // if X is out of boundary, fix it
        absMouse.x = 0;
    }

    if(absMouse.x > absMouse.maxX) {                                // if X is out of boundary, fix it
        absMouse.x = absMouse.maxX;
    }

    if(absMouse.y < 0) {                                            // if Y is out of boundary, fix it
        absMouse.y = 0;
    }

    if(absMouse.y > absMouse.maxY) {                                // if Y is out of boundary, fix it
        absMouse.y = absMouse.maxY;
    }
}

void Ikbd::processKeyboardData(bool skipKeyboardTranslation)
{
    uint8_t bfr[128];

    if(cur->keyboardData.count <= 0) {                             // no data? quit
        return;
    }

    while(cur->keyboardData.count > 0) {                           // while there are some data, process
        uint8_t val = cur->keyboardData.peek();                       // get the data, but don't move the get pointer, because we might fail later

        logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData -- got %02x, cb contains %d bytes", val, cur->keyboardData.count);

        if(val >= KEYBDATA_SPECIAL_LOWEST && val <= 0xff) {     // if this a special code?
            uint8_t index = val - KEYBDATA_SPECIAL_LOWEST;         // convert it to table index

            uint8_t len = specialCodeLen[index];                   // get length of the special sequence from table

            if(len > cur->keyboardData.count) {                    // not enough data in buffer? Maybe next time... Quit.
                return;
            }

            for(int i=0; i<len; i++) {                          // get the whole sequence
                bfr[i] = cur->keyboardData.get();
            }

			cur->ceIkbdMode = CE_IKBDMODE_INJECT;					// if we got at least one data sequence from original keyboard, switch to INJECT mode

			bool resendTheseData = true;						// reset this flag to not resend these data

            uint8_t stKeyboadCmd = bfr[0];                         // 0th byte is the original ST keyboard command code
            //---------------

			switch(stKeyboadCmd) {                              // handle data sent from original keyboard
				case KEYBDATA_MOUSE_ABS:						// report of absolute mouse position?
                logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData - keyboard says: KEYBDATA_MOUSE_ABS");

				if(gotUsbMouse()) {								// ...and we got USB mouse? Don't resend.
                    logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData - KEYBDATA_MOUSE_ABS -- got USB mouse, won't resend ABS pos from ST");
					resendTheseData = false;
				} else {
                    logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData - KEYBDATA_MOUSE_ABS -- no USB mouse, will resend ABS pos from ST");
                }

				break;

                case KEYBDATA_JOY0:
                if (firstJoyIs0 && joystickState == Enabled) {   // don't translate mouse in port0 as port1
                    bfr[0] = KEYBDATA_JOY1;
                } else if (firstJoyIs0) {
                    logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData - joy0->joy1 but not joystick mode is off, leaving as joy0");
                } else {
                    logIkbd(LOG_DEBUG, "firstJoy0: %d (joy0)", firstJoyIs0);
                }
                break;

                case KEYBDATA_JOY1:
                if (firstJoyIs0 && joystickState == Enabled) {   // don't map joy1 if joy0 is used as mouse
                    bfr[0] = KEYBDATA_JOY0;
                } else if (firstJoyIs0) {
                    logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData - joy1->joy0 but not joystick mode is off, leaving as joy1");
                } else {
                    logIkbd(LOG_DEBUG, "firstJoy0: %d (joy1)", firstJoyIs0);
                }
                break;

				//----------------------------------------------
				case KEYBDATA_JOY_BOTH:							// ST asked for both joystick states (interrogation) and this is the response
                logIkbd(LOG_DEBUG,  "Ikbd::processKeyboardData - keyboard says: KEYBDATA_JOY_BOTH");

                // Packet is 0xFD + joy0 state + joy1 state. Swap the ports of the real Atari joysticks like for JOY0 / JOY1
                // events above (rewriting bfr[0] to 0xFE / 0xFF would turn it into a 2-byte packet + 1 stray byte = key press on ST).
                if (firstJoyIs0 && joystickState == Enabled) {
                    uint8_t tmp = bfr[1];
                    bfr[1] = bfr[2];
                    bfr[2] = tmp;
                }

                // then USB joysticks override the port they are reported on (same ports as their JOY0 / JOY1 events)
                if(gotUsbJoy1()) {								// got joy 1?
                    uint8_t joy0state = joystick[0].lastDir | joystick[0].lastBtn;	// get state
					bfr[1] = joy0state;
				}

				if(gotUsbJoy2()) {								// got joy 2?
					uint8_t joy1state = joystick[1].lastDir | joystick[1].lastBtn;	// get state
					bfr[2] = joy1state;
				}

				break;
				//----------------------------------------------

			}

			if(resendTheseData) {								    // if we should resend this data
				sendToSt(bfr, len);                          // send the whole sequence to ST
            }

			continue;
        } else {                                                    // if it's not special IKBD code, then it's just a key press
            val = cur->keyboardData.get();                             // get data from buffer
            bool wasHandled = false;                       // it wasn't handled as keyb joy (yet)

            int pcKey = keyTranslator.stKeyToPc(val & 0x7f);
            skipKeyboardTranslation = handleHotkeys(pcKey, !(val & 0x80), skipKeyboardTranslation);

            if (!skipKeyboardTranslation && !wasHandled) {
                wasHandled = handleStKeyAsKeybJoy(val);    // get if it was handled as keyb joy
            }

            if(!wasHandled) {                              // if not handled as keyb joy, send it to ST
                sendToSt(&val, 1);             // send byte to ST
            }
        }
    }
}

void Ikbd::sendBothJoyReport(void)
{
	uint8_t bfr[3];

    uint8_t joy0state = joystick[0].lastDir | joystick[0].lastBtn;	// get state
	uint8_t joy1state = joystick[1].lastDir | joystick[1].lastBtn;	// get state

	bfr[0] = KEYBDATA_JOY_BOTH;

    bfr[1] = !firstJoyIs0 ? joy0state : joy1state;
    bfr[2] = !firstJoyIs0 ? joy1state : joy0state;

	sendToSt(bfr, 3);
}

// the following works when joystick event reporting is enabled
void Ikbd::sendJoyState(int joyNumber, int dirTotal)
{
    uint8_t bfr[2];

    // first set the joystick 0 / 1 tag
    if(joyNumber == 0) {        // joy 0
        bfr[0] = KEYBDATA_JOY0;
    } else {                    // joy 1
        bfr[0] = KEYBDATA_JOY1;
    }

    bfr[1] = dirTotal;

    sendToSt(bfr, 2);
}

// mouse buttons as IKBD reports them (bit 1 = left, bit 0 = right) - in mouse mode the joystick fire buttons act as mouse buttons
// (joy 0 fire = left, joy 1 fire = right), so both sources are merged - otherwise one would 'release' the other
uint8_t Ikbd::mouseButtonsWithJoy(void)
{
    uint8_t buttons = mouseBtnNow & 0x03;

    if(joystickState == EnabledInMouseMode) {
        if(joystick[0].lastBtn) {
            buttons |= 0x02;    // left mouse  button, joy0 button
        }
        if(joystick[1].lastBtn) {
            buttons |= 0x01;    // right mouse button, joy1 button
        }
    }

    return buttons;
}

// joystick fire changed while joystick works in mouse mode - report it as mouse button change
void Ikbd::sendJoyButtonsInMouseMode(void)
{
    if(mouseMode != MOUSEMODE_REL) {    // buttons as relative packet only make sense in relative mode
        return;
    }

    sendRelMousePackets(relMouse.accX, relMouse.accY);     // button change sends also the not yet reported movement
    relMouse.accX = 0;
    relMouse.accY = 0;
}

void Ikbd::fillStCommandsLengthTable(void)
{
    memset(stCommandLen, 0, 256);       // init table with zeros

    stCommandLen[STCMD_RESET]                           = STCMD_RESET_LEN;
    stCommandLen[STCMD_SET_MOUSE_BTN_ACTION]            = STCMD_SET_MOUSE_BTN_ACTION_LEN;
    stCommandLen[STCMD_SET_REL_MOUSE_POS_REPORTING]     = STCMD_SET_REL_MOUSE_POS_REPORTING_LEN;
    stCommandLen[STCMD_SET_ABS_MOUSE_POS_REPORTING]     = STCMD_SET_ABS_MOUSE_POS_REPORTING_LEN;
    stCommandLen[STCMD_SET_MOUSE_KEYCODE_MODE]          = STCMD_SET_MOUSE_KEYCODE_MODE_LEN;
    stCommandLen[STCMD_SET_MOUSE_THRESHOLD]             = STCMD_SET_MOUSE_THRESHOLD_LEN;
    stCommandLen[STCMD_SET_MOUSE_SCALE]                 = STCMD_SET_MOUSE_SCALE_LEN;
    stCommandLen[STCMD_INTERROGATE_MOUSE_POS]           = STCMD_INTERROGATE_MOUSE_POS_LEN;
    stCommandLen[STCMD_LOAD_MOUSE_POS]                  = STCMD_LOAD_MOUSE_POS_LEN;
    stCommandLen[STCMD_SET_Y_AT_BOTTOM]                 = STCMD_SET_Y_AT_BOTTOM_LEN;
    stCommandLen[STCMD_SET_Y_AT_TOP]                    = STCMD_SET_Y_AT_TOP_LEN;
    stCommandLen[STCMD_RESUME]                          = STCMD_RESUME_LEN;
    stCommandLen[STCMD_DISABLE_MOUSE]                   = STCMD_DISABLE_MOUSE_LEN;
    stCommandLen[STCMD_PAUSE_OUTPUT]                    = STCMD_PAUSE_OUTPUT_LEN;
    stCommandLen[STCMD_SET_JOYSTICK_EVENT_REPORTING]    = STCMD_SET_JOYSTICK_EVENT_REPORTING_LEN;
    stCommandLen[STCMD_SET_JOYSTICK_INTERROG_MODE]      = STCMD_SET_JOYSTICK_INTERROG_MODE_LEN;
    stCommandLen[STCMD_JOYSTICK_INTERROGATION]          = STCMD_JOYSTICK_INTERROGATION_LEN;
    stCommandLen[STCMD_SET_JOYSTICK_MONITORING]         = STCMD_SET_JOYSTICK_MONITORING_LEN;
    stCommandLen[STCMD_SET_FIRE_BUTTON_MONITORING]      = STCMD_SET_FIRE_BUTTON_MONITORING_LEN;
    stCommandLen[STCMD_SET_JOYSTICK_KEYCODE_MODE]       = STCMD_SET_JOYSTICK_KEYCODE_MODE_LEN;
    stCommandLen[STCMD_DISABLE_JOYSTICKS]               = STCMD_DISABLE_JOYSTICKS_LEN;
    stCommandLen[STCMD_TIMEOFDAY_CLOCK_SET]             = STCMD_TIMEOFDAY_CLOCK_SET_LEN;
    stCommandLen[STCMD_INTERROGATE_TIMEOFDAT_CLOCK]     = STCMD_INTERROGATE_TIMEOFDAT_CLOCK_LEN;
    stCommandLen[STCMD_MEMORY_LOAD]                     = STCMD_MEMORY_LOAD_LEN;
    stCommandLen[STCMD_MEMORY_READ]                     = STCMD_MEMORY_READ_LEN;
    stCommandLen[STCMD_CONTROLLER_EXECUTE]              = STCMD_CONTROLLER_EXECUTE_LEN;
}

void Ikbd::fillSpecialCodeLengthTable(void)
{
    specialCodeLen[KEYBDATA_STATUS		- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_STATUS_LEN;
    specialCodeLen[KEYBDATA_MOUSE_ABS	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_MOUSE_ABS_LEN;
    specialCodeLen[KEYBDATA_MOUSE_REL8	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_MOUSE_REL8_LEN;
    specialCodeLen[KEYBDATA_MOUSE_REL9	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_MOUSE_REL9_LEN;
    specialCodeLen[KEYBDATA_MOUSE_RELA	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_MOUSE_RELA_LEN;
    specialCodeLen[KEYBDATA_MOUSE_RELB	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_MOUSE_RELB_LEN;
    specialCodeLen[KEYBDATA_TIMEOFDAY	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_TIMEOFDAY_LEN;
    specialCodeLen[KEYBDATA_JOY_BOTH	- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_JOY_BOTH_LEN;
    specialCodeLen[KEYBDATA_JOY0		- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_JOY0_LEN;
    specialCodeLen[KEYBDATA_JOY1		- KEYBDATA_SPECIAL_LOWEST] = KEYBDATA_JOY1_LEN;
}

void Ikbd::sendMousePosAbsolute(uint8_t absButtons)
{
    if(!mouseEnabled) {                 // if mouse not enabled, don't send anything
        return;
    }

    if(mouseMode != MOUSEMODE_ABS) {    // if we're not in absolute mouse mode, quit
        return;
    }

	// create the absolute mouse position report
	uint8_t bfr[6];

	bfr[0] = KEYBDATA_MOUSE_ABS;
	bfr[1] = absButtons;
	bfr[2] = absMouse.x >> 8;
	bfr[3] = (uint8_t) absMouse.x;
	bfr[4] = absMouse.y >> 8;
	bfr[5] = (uint8_t) absMouse.y;

	sendToSt(bfr, 6);
}

// USB mouse moved - update absolute position, and report the movement in the way the current mouse mode needs
void Ikbd::mouseMoved(int dx, int dy)
{
    // absolute position is tracked in every mode (ST can switch to absolute mode and ask for it), scaled by mouse scale
    int scaleX = MAX(absMouse.scaleX, 1);
    int scaleY = MAX(absMouse.scaleY, 1);

    absMouse.fracX += dx;
    int stepsX = absMouse.fracX / scaleX;
    absMouse.fracX -= stepsX * scaleX;
    absMouse.x += stepsX;

    absMouse.fracY += mouseY0atTop ? dy : -dy;
    int stepsY = absMouse.fracY / scaleY;
    absMouse.fracY -= stepsY * scaleY;
    absMouse.y += stepsY;

    fixAbsMousePos();

    if(!mouseEnabled) {                     // mouse disabled - nothing is sent
        return;
    }

    if(mouseMode == MOUSEMODE_REL) {        // relative: send when movement reached the threshold in X or Y
        relMouse.accX += dx;
        relMouse.accY += dy;

        if(abs(relMouse.accX) >= MAX(relMouse.threshX, 1) || abs(relMouse.accY) >= MAX(relMouse.threshY, 1)) {
            sendRelMousePackets(relMouse.accX, relMouse.accY);
            relMouse.accX = 0;
            relMouse.accY = 0;
        }
        return;
    }

    if(mouseMode == MOUSEMODE_KEYCODE) {    // keycode: every deltaX / deltaY units of movement = one cursor key press
        int deltaX = MAX(keycodeMouse.deltaX, 1);
        int deltaY = MAX(keycodeMouse.deltaY, 1);

        keycodeMouse.accX += dx;
        keycodeMouse.accY += dy;

        while(abs(keycodeMouse.accX) >= deltaX) {
            bool right = keycodeMouse.accX > 0;
            sendKeyPress(right ? 0x4d : 0x4b);              // cursor RIGHT / LEFT
            keycodeMouse.accX += right ? -deltaX : deltaX;
        }

        while(abs(keycodeMouse.accY) >= deltaY) {
            bool down = keycodeMouse.accY > 0;
            sendKeyPress(down ? 0x50 : 0x48);               // cursor DOWN / UP
            keycodeMouse.accY += down ? -deltaY : deltaY;
        }
    }

    // absolute mode: nothing sent on movement, ST asks for position (or gets it on button press)
}

void Ikbd::sendKeyPress(uint8_t stKey)
{
    uint8_t bfr[2] = { stKey, (uint8_t) (stKey | 0x80) };  // make + break code
    sendToSt(bfr, 2);
}

// send relative mouse packet(s) with current buttons; movement bigger than one packet can hold (-128..127) is split
// into more packets, so it isn't wrapped around (e.g. +200 would become -56 = jump to the other side)
void Ikbd::sendRelMousePackets(int dx, int dy)
{
    if(!mouseEnabled) {                 // if mouse not enabled, don't send anything
        return;
    }

    if(mouseMode != MOUSEMODE_REL) {    // send relative mouse position changes only in relative mouse mode
        return;
    }

    if(!mouseY0atTop) {                 // if mouse Y=0 is at bottom, invert the Y axis (done on int, so -128 can't overflow)
        dy = -dy;
    }

    uint8_t buttons = mouseButtonsWithJoy();

    do {                                // at least one packet - also for button change without movement
        int x = MAX(-128, MIN(127, dx));
        int y = MAX(-128, MIN(127, dy));

        uint8_t bfr[3];
        bfr[0] = 0xf8 | buttons;
        bfr[1] = (uint8_t) x;
        bfr[2] = (uint8_t) y;
        sendToSt(bfr, 3);

        dx -= x;
        dy -= y;
    } while(dx != 0 || dy != 0);
}

bool Ikbd::handleStKeyAsKeybJoy(uint8_t val)
{
    bool keyDown    = ((val & 0x80) == 0);      // if highest bit is zero, it's key down event
    uint8_t stKey      =   val & 0x7f;             // get just the key, without highest bit

    bool isKeybJoy0 = keyJoyKeys.isKeybJoyKeyAtari(0, stKey);       // find out if it bellongs to joy0
    bool isKeybJoy1 = keyJoyKeys.isKeybJoyKeyAtari(1, stKey);       // find out if it bellongs to joy1

    if(isKeybJoy0 && !keybJoy0) {               // it's keyb joy 0, but keyb joy 0 is not enabled, fail
        return false;
    }

    if(isKeybJoy1 && !keybJoy1) {               // it's keyb joy 1, but keyb joy 1 is not enabled, fail
        return false;
    }

    if(!isKeybJoy0 && !isKeybJoy1) {            // it's not from any keyb joy, quit
        return false;
    }

    int joyNumber = isKeybJoy0 ? 0 : 1;         // if it's from joy 0, then joy # is 0, otherwise 1

    handleKeyAsKeybJoy(false, joyNumber, stKey, keyDown);   // handle this key press, and it comes from ST keys (therefore first param: false)
    return true;
}
