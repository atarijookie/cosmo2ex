// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/select.h>
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
#include "../discovery/discovery.h"

#include "ikbd.h"
#include "inputsocket.h"
#include "ikbdinputsink.h"
#include "inputdevices.h"


TInputDevice ikbdDevs[INTYPE_COUNT];

void processInputFromAttachedDevices(Ikbd& ikbd, fd_set* pReadfds)
{
    for(int i = 0; i < INTYPE_COUNT; i++) {         // go through the input devices
        int fd = ikbd.getFdByIndex(i);

        if(fd < 0 || !FD_ISSET(fd, pReadfds)) {     // not open or not set? skip
            continue;
        }

        bool isJoystick = (i == INTYPE_JOYSTICK1 || i == INTYPE_JOYSTICK2);
        int  eventSize  = isJoystick ? (int) sizeof(js_event) : (int) sizeof(input_event);

        // read the (rest of the) event - a read can return less than a whole event, then the rest comes later
        TInputDevice *dev = &ikbdDevs[i];
        ssize_t res = read(fd, dev->partial + dev->partialLen, eventSize - dev->partialLen);

        if(res < 0) {                                           // on error, skip the rest
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {     // nothing to read now
                continue;
            }

            if(errno == ENODEV) {                               // if device was removed, deinit it
                ikbd.deinitDev(i);
            } else {
                logIkbd(LOG_ERROR, "ikbdThreadCode() read(%d) : %s", fd, strerror(errno));
            }
            continue;
        }

        if(res == 0) {                                          // EOF
            logIkbd(LOG_ERROR, "ikbdThreadCode() read(%d) returned 0 (EOF) closing %d", fd, i);
            ikbd.deinitDev(i);
            continue;
        }

        dev->partialLen += res;

        if(dev->partialLen < eventSize) {                       // not whole event yet? wait for the rest
            continue;
        }

        dev->partialLen = 0;                                    // got whole event, next read starts a new one

        if(isJoystick) {
            js_event js;
            memcpy(&js, dev->partial, sizeof(js));
            ikbd.processJoystick(&js, i - INTYPE_JOYSTICK1);
            continue;
        }

        input_event ev;
        memcpy(&ev, dev->partial, sizeof(ev));

        switch(i) {
            case INTYPE_MOUSE:
                ikbd.processMouse(&ev);                         // then process the event
                break;
            case INTYPE_KEYBOARD:
                ikbd.processKeyboard(&ev, false);               // false: don't skip keyboard translation
                break;
        }
    }
}

void handleClients(Ikbd& ikbd, fd_set* pReadfds, ChipInterface* ciIkbd)
{
    for(int i=0; i<MAX_CLIENTS; i++) {
        ClientInfo* ci = ciIkbd->clientGetByIndex(i);

        if(ci->fdClient == FD_EMPTY) {           // no client here? skip it
            continue;
        }

        if(FD_ISSET(ci->fdClient, pReadfds)) {           // this fd read for read?
            // process the incoming data from original keyboard and from ST
            int bytesRead = ikbd.processReceivedCommands(false, ci->fdClient);    // false: don't skip keyboard translation

            if(bytesRead < 0) {     // client closed the connection (or it broke)? close it now, otherwise select() reports it readable in every loop
                logIkbd(LOG_INFO, "handleClients - client #%d closed connection", i);
                ciIkbd->closeClientByFd(ci->fdClient);
                continue;
            }

            if(bytesRead > 0) {     // something was read, mark client as active
                ci->lastMs = Utils::getCurrentMs();
            }
        }
    }
}

void *ikbdThreadCode(void *ptr)
{
    int max_fd;
    int fd;
    fd_set readfds;
    /*struct timeval timeout;*/
    int i;

    logIkbd(LOG_DEBUG, "----------------------------------------------------------");
    logIkbd(LOG_DEBUG, "ikbdThreadCode will enter loop...");

    // create network chip interface
    ChipInterface* ciIkbd = new ChipInterface(LOGFILE_IKBD, 0, 0, SERVER_TCP_PORT_IKBD);
    if(!ciIkbd->ciOpen()) {                // can't listen on our tcp port? we can't serve any data, so terminate the app
        fatalErrorOccurred = 1;
        sigintReceived = 1;
    }

    Ikbd ikbd(ciIkbd);

    InputDevWatch devWatch;                 // USB input devices attached later
    devWatch.open();
    ikbd.findDevices();                     // and those attached now

    IkbdInputSink inputSink(ikbd);
    InputSocket inputSocket(&inputSink);    // keyboard / mouse / joystick events from external app, see inputsocket.h
    inputSocket.open();                     // failure is logged, the rest works without it

    while(sigintReceived == 0) {
        Utils::sleepMs(1);      // intentional sleep to not utilize cpu to max when looping too much before client disconnect

        // reload config if needed
        if(events.ikbdReloadConfig) {
            events.ikbdReloadConfig = false;
            ikbd.loadSettings();
        }

        ciIkbd->clientsDisconnectInactive();

        max_fd = -1;
        FD_ZERO(&readfds);
        for(i = 0; i < INTYPE_COUNT; i++) {                                       // go through the input devices
            fd = ikbd.getFdByIndex(i);
            if(fd >= 0) {
                FD_SET(fd, &readfds);
                max_fd = MAX(max_fd, fd);
            }
        }

        // get listening socket from chip interface, add it to readfds
        int fdListen = ciIkbd->getFdListen();
        FD_SET(fdListen, &readfds);
        max_fd = MAX(max_fd, fdListen);

        // get all connected client fds, add them to readfds
        max_fd = MAX(max_fd, ciIkbd->setAllClientFds(&readfds));     // all valid client fds will be set to readfds, and highest fd into max_fd

        max_fd = MAX(max_fd, inputSocket.setFds(&readfds));
        max_fd = MAX(max_fd, devWatch.setFds(&readfds));

        // add timeout to select(), so we can check for connection status, settings reload, etc.
        timeval timeout;
        memset(&timeout, 0, sizeof(timeout));
        timeout.tv_sec = 2;

        if(select(max_fd + 1, &readfds, NULL, NULL, &timeout) < 0) {
            if(errno == EINTR) {
                continue;   // a signal was delivered
            } else {
                logIkbd(LOG_ERROR, "ikbdThreadCode() select: %s", strerror(errno));
                continue;
            }
        }

        devWatch.handle(&readfds, ikbd);

        // if listening socket is set, handle it
        if(FD_ISSET(fdListen, &readfds)) {
            int idx = ciIkbd->acceptSocketIfNeededAndPossible();

            if(idx != FD_EMPTY) {               // new device in this slot - its own buffers / modes, nothing left from the previous one
                ikbd.clientConnected(idx);
            }
        }

        // process the incoming data from original keyboard and from ST
        handleClients(ikbd, &readfds, ciIkbd);

        // process events from attached input devices
        processInputFromAttachedDevices(ikbd, &readfds);

        // and from the app on the input socket
        inputSocket.handle(&readfds);
    }

    inputSocket.close();                    // releases what the app still holds - before ikbd goes away

    devWatch.close();
    ikbd.closeDevs();

    ciIkbd->ciClose();
    delete ciIkbd;
    ciIkbd = NULL;

    logIkbd(LOG_DEBUG, "ikbdThreadCode has quit");
    return 0;
}
