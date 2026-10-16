// Test-side control of the fake platform (clock, alarms, core number, debug output, EEPROM).

#ifndef FAKES_H
#define FAKES_H

#include <string>
#include "pico_fake.h"

extern uint32_t    fakeNow;             // current fake time in ms
extern uint32_t    fakeMillisStep;      // added to fakeNow on every millis() call, so busy-wait loops can time out
extern uint        fakeCoreNum;         // what get_core_num() returns
extern uint        fakeException;       // what __get_current_exception() returns, non-zero = code runs in IRQ handler
extern uint8_t     fakeBoardId[8];      // what pico_get_unique_board_id() returns
extern std::string fakeDebugOut;        // everything sent to the debug uart
extern uint32_t    fakeLastAlarmMs;     // duration of the last add_alarm_in_ms()
extern int         fakeActiveAlarms;    // alarms added and not yet fired / cancelled
extern int         fakeLockoutCount;    // multicore_lockout_start_blocking() calls

extern bool             fakeAlarmAddFails;      // add_alarm_in_ms() adds nothing and returns fakeAlarmAddResult instead
extern alarm_id_t       fakeAlarmAddResult;     // 0 = the alarm time has already passed, < 0 = no free alarm slot
extern bool             fakeAlarmFireDuringAdd; // the alarm fires before add_alarm_in_ms() returns, as when the calling core gets stalled
extern alarm_callback_t fakeLastAlarmCallback;  // callback + user data of the last added alarm, so a test can deliver it late
extern void            *fakeLastAlarmUserData;

void fakeAdvanceMillis(uint32_t ms);    // moves the clock, fires due alarms
void fakeReset(void);                   // resets everything except the clock (the code under test keeps static timestamps)

#endif
