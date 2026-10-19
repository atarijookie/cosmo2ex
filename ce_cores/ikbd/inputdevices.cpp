// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <libudev.h>
#include <libevdev/libevdev.h>
#endif

#include "../misc/debug.h"
#include "ikbd.h"
#include "linuxinput.h"
#include "inputdevices.h"

extern TInputDevice ikbdDevs[INTYPE_COUNT];
extern bool noCapture;

#if defined(__linux__)

int evdevInputType(const struct libevdev *evdev)
{
    // mouse first - some keyboards with touchpad / trackball (e.g. Logitech K400) are a single device with keys and
    // relative movement; as a mouse their keys still work, processMouse() passes them to the keyboard code
    if(libevdev_has_event_code(evdev, EV_REL, REL_X) && libevdev_has_event_code(evdev, EV_REL, REL_Y) &&
       libevdev_has_event_code(evdev, EV_KEY, BTN_LEFT)) {
        return INTYPE_MOUSE;
    }

    // keyboard - has the typing keys (not just a few like power button, media keys or a remote control)
    const int typingKeys[] = {KEY_ESC, KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER};
    for(int key : typingKeys) {
        if(!libevdev_has_event_code(evdev, EV_KEY, key)) {
            return -1;
        }
    }
    return INTYPE_KEYBOARD;
}

void inputDevClose(TInputDevice& dev)
{
    if(dev.evdev) {
        libevdev_free(dev.evdev);       // doesn't close the fd
        dev.evdev = NULL;
    }

    if(dev.fd != -1) {
        close(dev.fd);
        dev.fd = -1;
    }
}

void Ikbd::findDevices(void)
{
    struct udev *udev = udev_new();
    if(!udev) {
        logIkbd(LOG_ERROR, "Ikbd::findDevices() - udev_new() failed");
        return;
    }

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "input");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
        if(!dev) {
            continue;
        }

        const char *devNode = udev_device_get_devnode(dev);     // NULL for the parent 'inputN' devices
        if(devNode) {
            deviceAttached(devNode);
        }
        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
    udev_unref(udev);
}

void Ikbd::deviceAttached(const char *devNode)
{
    const char *name = strrchr(devNode, '/');
    name = name ? name + 1 : devNode;

    bool isJoy   = (strncmp(name, "js", 2) == 0);           // joydev - only joysticks / gamepads have these
    bool isEvdev = (strncmp(name, "event", 5) == 0);

    if(!isJoy && !isEvdev) {                                // e.g. mouseN, mice - same devices as eventN, in older format
        return;
    }

    for(int i=0; i<INTYPE_COUNT; i++) {                     // already have it (found by scan and reported by watch too)
        if(strcmp(ikbdDevs[i].devPath, devNode) == 0) {
            return;
        }
    }

    if(strlen(devNode) >= sizeof(ikbdDevs[0].devPath)) {    // path wouldn't fit to devPath
        logIkbd(LOG_ERROR, "Input device path too long (max %d chars): %s", (int) sizeof(ikbdDevs[0].devPath) - 1, devNode);
        return;
    }

    int index = -1;
    if(isJoy) {
        // fixed slots: 1st stick -> joystick[1] (port 1 by default), 2nd stick -> joystick[0] (port 0 by default);
        // swapping ports (firstJoyIs0) is done when sending, see joyPort(), so it also applies to already connected sticks
        if(ikbdDevs[INTYPE_JOYSTICK2].fd == -1) {
            index = INTYPE_JOYSTICK2;
        } else if(ikbdDevs[INTYPE_JOYSTICK1].fd == -1) {
            index = INTYPE_JOYSTICK1;
        } else {
            logIkbd(LOG_INFO, "%s: already have both joysticks", devNode);
            return;
        }
    }

    int fd = open(devNode, O_RDONLY | O_NONBLOCK);
    if(fd < 0) {        // warning only - every input device is opened to find out what it is, not just keyboard / mouse
        logIkbd(LOG_WARNING, "Failed to open input device %s : %s", devNode, strerror(errno));
        return;
    }

    struct libevdev *evdev = NULL;
    const char *what;

    if(isJoy) {
        what = (index == INTYPE_JOYSTICK2) ? "joystick1" : "joystick2";
    } else {
        int rc = libevdev_new_from_fd(fd, &evdev);
        if(rc < 0) {
            logIkbd(LOG_ERROR, "Failed to init evdev of input device %s : %s", devNode, strerror(-rc));
            close(fd);
            return;
        }

        index = evdevInputType(evdev);
        if(index < 0) {                                     // not keyboard / mouse - e.g. power button, speaker, webcam button
            logIkbd(LOG_DEBUG, "%s (%s): not keyboard / mouse, ignored", devNode, libevdev_get_name(evdev));
            libevdev_free(evdev);
            close(fd);
            return;
        }

        what = (index == INTYPE_KEYBOARD) ? "keyboard" : "mouse";

        if(ikbdDevs[index].fd != -1) {
            logIkbd(LOG_INFO, "%s (%s): already have a %s", devNode, libevdev_get_name(evdev), what);
            libevdev_free(evdev);
            close(fd);
            return;
        }
    }

    TInputDevice *in = &ikbdDevs[index];
    in->fd = fd;
    in->evdev = evdev;
    in->partialLen = 0;
    snprintf(in->devPath, sizeof(in->devPath), "%s", devNode);
    logIkbd(LOG_INFO, "Got device (%s): %s %s", what, devNode, evdev ? libevdev_get_name(evdev) : "");

    // grab only keyboard / mouse (evdev) - joysticks are /dev/input/js* where grab doesn't work
    if(evdev && keyboardExclusiveAccess) {
        grabExclusiveAccess(index);
    }
}

void Ikbd::deviceRemoved(const char *devNode)
{
    for(int i=0; i<INTYPE_COUNT; i++) {
        if(strcmp(ikbdDevs[i].devPath, devNode) == 0) {     // usually closed already, when reading from it failed
            logIkbd(LOG_INFO, "Input device removed: %s", devNode);
            deinitDev(i);
        }
    }

    findDevices();      // another keyboard / mouse / joystick might be attached, which we skipped while we had this one
}

bool Ikbd::grabExclusiveAccess(int index)
{
    if(noCapture) {       // if shouldn't do USB keyboard and mouse capture, just quit
        return false;
    }

    struct libevdev *evdev = ikbdDevs[index].evdev;
    if(!evdev) {          // not open, or not evdev device
        return false;
    }

    int rc = libevdev_grab(evdev, LIBEVDEV_GRAB);
    if(rc < 0) {
        logIkbd(LOG_ERROR, "Ikbd::grabExclusiveAccess() failed : %s", strerror(-rc));
        return false;
    }

    return true;
}

void Ikbd::releaseExclusiveAccess(int index)
{
    struct libevdev *evdev = ikbdDevs[index].evdev;
    if(!evdev) {          // not open, or not evdev device
        return;
    }

    int rc = libevdev_grab(evdev, LIBEVDEV_UNGRAB);
    if(rc < 0) {
        logIkbd(LOG_ERROR, "Ikbd::releaseExclusiveAccess() failed : %s", strerror(-rc));
    }
}

InputDevWatch::InputDevWatch() : udev(NULL), monitor(NULL)
{
}

InputDevWatch::~InputDevWatch()
{
    close();
}

void InputDevWatch::open(void)
{
    udev = udev_new();
    if(!udev) {
        logIkbd(LOG_ERROR, "InputDevWatch - udev_new() failed");
        return;
    }

    // "udev" events come after udev set up the device (permissions, links), but only while udevd runs - which might
    // not be the case e.g. in a container without /run/udev. Then take the events straight from the kernel, the
    // device node is already there (devtmpfs) when they come.
    const char *source = (access("/run/udev/control", F_OK) == 0) ? "udev" : "kernel";

    monitor = udev_monitor_new_from_netlink(udev, source);
    if(!monitor) {
        logIkbd(LOG_ERROR, "InputDevWatch - udev_monitor_new_from_netlink(%s) failed", source);
        close();
        return;
    }

    udev_monitor_filter_add_match_subsystem_devtype(monitor, "input", NULL);

    if(udev_monitor_enable_receiving(monitor) < 0) {
        logIkbd(LOG_ERROR, "InputDevWatch - udev_monitor_enable_receiving() failed");
        close();
        return;
    }

    logIkbd(LOG_INFO, "InputDevWatch - watching for input devices (%s events)", source);
}

int InputDevWatch::setFds(fd_set *fds)
{
    if(!monitor) {
        return -1;
    }

    int fd = udev_monitor_get_fd(monitor);
    FD_SET(fd, fds);
    return fd;
}

void InputDevWatch::handle(fd_set *fds, Ikbd& ikbd)
{
    if(!monitor || !FD_ISSET(udev_monitor_get_fd(monitor), fds)) {
        return;
    }

    struct udev_device *dev = udev_monitor_receive_device(monitor);
    if(!dev) {
        return;
    }

    const char *action  = udev_device_get_action(dev);
    const char *devNode = udev_device_get_devnode(dev);     // NULL for the parent 'inputN' devices

    if(action && devNode) {
        logIkbd(LOG_DEBUG, "InputDevWatch - %s %s", action, devNode);

        if(strcmp(action, "add") == 0) {
            ikbd.deviceAttached(devNode);
        } else if(strcmp(action, "remove") == 0) {
            ikbd.deviceRemoved(devNode);
        }
    }

    udev_device_unref(dev);
}

void InputDevWatch::close(void)
{
    if(monitor) {
        udev_monitor_unref(monitor);
        monitor = NULL;
    }

    if(udev) {
        udev_unref(udev);
        udev = NULL;
    }
}

#else

// no USB keyboard / mouse / joysticks on this system - they come only through the input socket (see inputsocket.h)

void inputDevClose(TInputDevice& dev)
{
    if(dev.fd != -1) {
        close(dev.fd);
        dev.fd = -1;
    }
}

void Ikbd::findDevices(void)                                { }
void Ikbd::deviceAttached(const char *devNode)              { }
void Ikbd::deviceRemoved(const char *devNode)               { }
bool Ikbd::grabExclusiveAccess(int index)                   { return false; }
void Ikbd::releaseExclusiveAccess(int index)                { }

InputDevWatch::InputDevWatch() : udev(NULL), monitor(NULL)  { }
InputDevWatch::~InputDevWatch()                             { }
void InputDevWatch::open(void)                              { }
int  InputDevWatch::setFds(fd_set *fds)                     { return -1; }
void InputDevWatch::handle(fd_set *fds, Ikbd& ikbd)         { }
void InputDevWatch::close(void)                             { }

#endif
