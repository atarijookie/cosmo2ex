// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef _INPUTDEVICES_H_
#define _INPUTDEVICES_H_

#include <sys/select.h>

/*
    USB (and Bluetooth) keyboard, mouse and joysticks attached to this machine - Linux only.

    Devices are found with libudev (input subsystem) and inspected with libevdev:
        - /dev/input/event* is a keyboard when it has the typing keys, a mouse when it has relative X / Y and left button
          (decided from its capabilities, not from its name)
        - /dev/input/js* is a joystick - the kernel (joydev) creates these only for joysticks / gamepads
    InputDevWatch reports devices attached / removed later (udev monitor).

    Elsewhere there are no such devices - keyboard, mouse and joysticks come only through the input socket (see inputsocket.h) -
    so finding devices does nothing and InputDevWatch never reports anything.
*/

class Ikbd;

class InputDevWatch
{
public:
    InputDevWatch();
    ~InputDevWatch();

    void open(void);                        // start watching - before Ikbd::findDevices(), so no device attached in between is missed
    int  setFds(fd_set *fds);               // add own fd to fds, returns it (-1 if none)
    void handle(fd_set *fds, Ikbd& ikbd);   // pass attached / removed devices to ikbd
    void close(void);

private:
    struct udev         *udev;
    struct udev_monitor *monitor;
};

#if defined(__linux__)
struct libevdev;

// INTYPE_KEYBOARD or INTYPE_MOUSE by what the evdev device can do, -1 if it's neither (public for tests)
int evdevInputType(const struct libevdev *evdev);
#endif

#endif
