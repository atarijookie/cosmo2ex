// core1_main.cpp - read data output enable (PIN_FLCC_OE, active low): on only while our drive is selected and the motor
// is on. The bus is shared with another drive, so it must follow DRIVE_SEL / MOT_EN right away - in the GPIO IRQ, not in
// a main loop that can be busy for hundreds of ms (it used to be in the core0 loop, which waits on WiFi / TCP).

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "ipc.h"
#include "core1_main.h"

extern volatile IPCbuffer* wrBuffer;            // core1_main.cpp - set while a write is captured

namespace {

bool outputEnabled(void)
{
    return !fakeGpioLevel[PIN_FLCC_OE];
}

struct OutputEnableFixture {
    OutputEnableFixture() {
        fakeReset();
        fakeGpioLevel[PIN_FLCC_OE] = true;      // disabled, as core0 sets it on start
        fakeGpioLevel[PIN_DRIVE_SEL] = true;    // not selected
        fakeGpioLevel[PIN_MOT_EN] = true;       // motor off
    }
};

}   // namespace

TEST_SUITE("fdd output enable") {

TEST_CASE_FIXTURE(OutputEnableFixture, "output follows DRIVE_SEL and MOT_EN on every edge, without any main loop") {
    streamOutputEnableSetup();
    CHECK_FALSE(outputEnabled());

    fakeGpioSet(PIN_DRIVE_SEL, false);          // our drive selected, motor still off
    CHECK_FALSE(outputEnabled());

    fakeGpioSet(PIN_MOT_EN, false);             // motor on
    CHECK(outputEnabled());

    fakeGpioSet(PIN_DRIVE_SEL, true);           // ST selects the other drive - it sends data now, we must not
    CHECK_FALSE(outputEnabled());

    fakeGpioSet(PIN_DRIVE_SEL, false);          // back to our drive
    CHECK(outputEnabled());

    fakeGpioSet(PIN_MOT_EN, true);              // motor off
    CHECK_FALSE(outputEnabled());
}

TEST_CASE_FIXTURE(OutputEnableFixture, "already selected with motor on at setup - output enabled right away") {
    fakeGpioLevel[PIN_DRIVE_SEL] = false;
    fakeGpioLevel[PIN_MOT_EN] = false;

    streamOutputEnableSetup();

    CHECK(outputEnabled());
}

TEST_CASE_FIXTURE(OutputEnableFixture, "edges merged into one IRQ - output set from the current levels") {
    streamOutputEnableSetup();

    fakeGpioLevel[PIN_DRIVE_SEL] = false;       // selected and deselected again before the IRQ got handled
    fakeGpioLevel[PIN_MOT_EN] = false;
    fakeGpioLevel[PIN_DRIVE_SEL] = true;
    fakeGpioIrq(PIN_DRIVE_SEL, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);

    CHECK_FALSE(outputEnabled());
}

TEST_CASE_FIXTURE(OutputEnableFixture, "output enable and write capture share the core1 GPIO callback - both keep working") {
    ipcInit();
    fakeGpioLevel[PIN_WGATE] = true;            // not writing
    writeCaptureSetup();                        // sets the same callback for WGATE
    streamOutputEnableSetup();

    fakeGpioSet(PIN_DRIVE_SEL, false);
    fakeGpioSet(PIN_MOT_EN, false);
    CHECK(outputEnabled());

    fakeGpioSet(PIN_WGATE, false);              // write start reaches the WGATE handler
    CHECK(wrBuffer != NULL);
    fakeGpioSet(PIN_WGATE, true);
    CHECK(wrBuffer == NULL);
}

}
