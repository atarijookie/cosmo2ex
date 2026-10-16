// State and implementation of the fake platform, see fakes.h

#include <vector>
#include "fakes.h"
#include "Arduino.h"
#include "EEPROM.h"
#include "Ethernet.h"

uint32_t    fakeNow          = 1000000;     // not 0, the code under test uses 'now - lastAttempt' with lastAttempt preset to -65k
uint32_t    fakeMillisStep   = 0;
uint        fakeCoreNum      = 0;
uint        fakeException    = 0;
static const uint8_t defaultBoardId[8] = { 0xe6, 0x61, 0x38, 0x52, 0x83, 0x2f, 0x4d, 0x2a };
uint8_t     fakeBoardId[8];
std::string fakeDebugOut;
uint32_t    fakeLastAlarmMs  = 0;
int         fakeActiveAlarms = 0;
int         fakeLockoutCount = 0;

bool             fakeAlarmAddFails      = false;
alarm_id_t       fakeAlarmAddResult     = 0;
bool             fakeAlarmFireDuringAdd = false;
alarm_callback_t fakeLastAlarmCallback  = nullptr;
void            *fakeLastAlarmUserData  = nullptr;

FakeEEPROM    EEPROM;
EthernetClass Ethernet;

FakeIoBank0   fakeIoBank0;
FakePadsBank0 fakePadsBank0;
FakeSio       fakeSio;

struct FakeAlarm {
    alarm_id_t       id;
    uint32_t         due;
    alarm_callback_t callback;
    void            *userData;
    bool             active;
};

static std::vector<FakeAlarm> alarms;
static alarm_id_t nextAlarmId = 1;

static void fireDueAlarms(void)
{
    for (size_t i = 0; i < alarms.size(); i++) {
        FakeAlarm &a = alarms[i];

        if (a.active && (int32_t)(fakeNow - a.due) >= 0) {
            a.active = false;
            fakeActiveAlarms--;
            a.callback(a.id, a.userData);       // one-shot only, the code under test always returns 0
        }
    }
}

void fakeAdvanceMillis(uint32_t ms)
{
    fakeNow += ms;
    fireDueAlarms();
}

void fakeReset(void)
{
    fakeMillisStep   = 0;
    fakeCoreNum      = 0;
    fakeException    = 0;
    memcpy(fakeBoardId, defaultBoardId, sizeof(fakeBoardId));
    fakeDebugOut.clear();
    fakeLastAlarmMs  = 0;
    fakeActiveAlarms = 0;
    fakeLockoutCount = 0;
    fakeAlarmAddFails      = false;
    fakeAlarmAddResult     = 0;
    fakeAlarmFireDuringAdd = false;
    fakeLastAlarmCallback  = nullptr;
    fakeLastAlarmUserData  = nullptr;
    alarms.clear();
    EEPROM.reset();
    Ethernet = EthernetClass();
}

//--------------------------------------------------------

uint32_t millis(void)
{
    if (fakeMillisStep) {
        fakeAdvanceMillis(fakeMillisStep);
    }
    return fakeNow;
}

void delay(uint32_t ms)    { fakeAdvanceMillis(ms); }
void sleep_ms(uint32_t ms) { fakeAdvanceMillis(ms); }

alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t callback, void *user_data, bool)
{
    fakeLastAlarmMs       = ms;
    fakeLastAlarmCallback = callback;
    fakeLastAlarmUserData = user_data;

    if (fakeAlarmAddFails) {
        return fakeAlarmAddResult;
    }

    FakeAlarm a = { nextAlarmId++, fakeNow + ms, callback, user_data, !fakeAlarmFireDuringAdd };

    if (fakeAlarmFireDuringAdd) {       // already fired, nothing left to cancel
        callback(a.id, user_data);
    } else {
        fakeActiveAlarms++;
    }

    alarms.push_back(a);
    return a.id;
}

bool cancel_alarm(alarm_id_t id)
{
    for (FakeAlarm &a : alarms) {
        if (a.id == id && a.active) {
            a.active = false;
            fakeActiveAlarms--;
            return true;
        }
    }
    return false;
}

uint get_core_num(void) { return fakeCoreNum; }
uint __get_current_exception(void) { return fakeException; }

void pico_get_unique_board_id(pico_unique_board_id_t *id_out) { memcpy(id_out->id, fakeBoardId, sizeof(id_out->id)); }

void multicore_lockout_start_blocking(void) { fakeLockoutCount++; }
void multicore_lockout_end_blocking(void) {}

//--------------------------------------------------------

void queue_init(queue_t *q, uint element_size, uint element_count)
{
    q->data        = (uint8_t *) calloc(element_count, element_size);
    q->elementSize = element_size;
    q->capacity    = element_count;
    q->head        = 0;
    q->count       = 0;
}

void queue_free(queue_t *q)
{
    free(q->data);
    q->data = nullptr;
}

bool queue_try_add(queue_t *q, const void *data)
{
    if (q->count >= q->capacity) return false;

    uint tail = (q->head + q->count) % q->capacity;
    memcpy(q->data + tail * q->elementSize, data, q->elementSize);
    q->count++;
    return true;
}

bool queue_try_remove(queue_t *q, void *data)
{
    if (q->count == 0) return false;

    memcpy(data, q->data + q->head * q->elementSize, q->elementSize);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

bool queue_is_empty(queue_t *q)  { return q->count == 0; }
uint queue_get_level(queue_t *q) { return q->count; }

//--------------------------------------------------------

bool pio_claim_free_sm_and_add_program_for_gpio_range(const pio_program *, PIO *pio, uint *sm, uint *offset, uint, uint, bool)
{
    *pio    = nullptr;
    *sm     = 0;
    *offset = 0;
    return true;
}

void fakeUartPutc(char c) { fakeDebugOut += c; }
