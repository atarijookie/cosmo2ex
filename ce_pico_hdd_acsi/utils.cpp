#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/unique_id.h"

#include <EEPROM.h>

#include "utils.h"
#include "defs.h"
#include "display.h"

#ifdef LOG_LED
#include "uart_tx.pio.h"

PIO pioUartTx;
uint smUartTx;

queue_t fifoDebug;
#endif

volatile bool hasTimedOut = false;
volatile uint8_t timerRunning = false;
alarm_id_t timer_id = 0;    // Will hold the alarm handle
volatile uint32_t timerGeneration = 0;  // changed on each timeout start / clear, so a late callback of an old alarm can be recognized

mutex_t debugMutex;

struct TSettings settings;

uint16_t getWord(uint8_t *bfr)
{
    uint16_t val = 0;

    val = bfr[0];       // get hi
    val = val << 8;

    val |= bfr[1];      // get lo

    return val;
}

uint32_t getDword(uint8_t *bfr)
{
    uint32_t val = 0;

    val = bfr[0];       // get hi
    val = val << 8;

    val |= bfr[1];      // get mid hi
    val = val << 8;

    val |= bfr[2];      // get mid lo
    val = val << 8;

    val |= bfr[3];      // get lo

    return val;
}

uint32_t get24bits(uint8_t *bfr)
{
    uint32_t val = 0;

    val  = bfr[0];       // get hi
    val  = val << 8;

    val |= bfr[1];      // get mid
    val  = val << 8;

    val |= bfr[2];      // get lo

    return val;
}

void storeWord(uint8_t *bfr, uint16_t val)
{
    bfr[0] = val >> 8;  // store hi
    bfr[1] = val;       // store lo
}

void storeDword(uint8_t *bfr, uint32_t val)
{
    bfr[0] = val >> 24; // store hi
    bfr[1] = val >> 16; // store mid hi
    bfr[2] = val >>  8; // store mid lo
    bfr[3] = val;       // store lo
}

void store24bits(uint8_t *bfr, uint32_t val)
{
    bfr[0] = val >> 16;
    bfr[1] = val >>  8;
    bfr[2] = val;
}

int64_t timer_callback(alarm_id_t id, void *user_data) {
    if((uint32_t)(uintptr_t) user_data != timerGeneration) {    // callback of an alarm which was cancelled meanwhile (it runs on the other core), ignore
        return 0;
    }

    hasTimedOut = true;
    timerRunning = false;
    return 0;   // 0 = do NOT reschedule (one-shot)
}

void timeoutStart(uint32_t durationMs)
{
// #ifdef LOG_MORE
//     debug("timeoutStart %d at %d", durationMs, millis());
// #endif

    timeoutClear();             // if already running, cancel old one

    // set the flags before adding the alarm - if this core gets stalled (e.g. by flash lockout), the alarm might fire before add_alarm_in_ms() returns
    hasTimedOut = false;
    timerRunning = true;

    alarm_id_t id = add_alarm_in_ms(durationMs, timer_callback, (void *)(uintptr_t) timerGeneration, false);

    if(id <= 0) {               // no free alarm slot (< 0) or the time has already passed (0) - time out now instead of waiting forever
        hasTimedOut = true;
        timerRunning = false;
        return;
    }

    timer_id = id;
}

void timeoutClear(void)
{
// #ifdef LOG_MORE
//  debug("timeoutClear\n");
// #endif

    timerGeneration++;          // if the alarm callback is just running on the other core, it will be ignored

    if (timerRunning) {
        timerRunning = false;
        cancel_alarm(timer_id);
    }

    hasTimedOut = false;
    timerRunning = false;
}

void cmdTimeoutChangeLength(uint32_t newPeriod)
{
    timeoutClear();
    timeoutStart(newPeriod);
}

void longTimeout_basedOnSectorCount(uint16_t sectorCount)
{
    uint32_t mbCount       = (sectorCount >> 11) + 1;                  // convert sector count into megabytes (rounded up)
    uint32_t timeoutSecs   = mbCount       * CMD_TIMEOUT_SECS_PER_MB;  // convert MBs into seconds
    uint32_t timeoutPeriod = timeoutSecs   * CMD_TIMEOUT_ONESECOND;    // and convert seconds into ms

    uint32_t timeoutDuration = MIN(timeoutPeriod, 30000);        // Now limit the timeout period to 30 s
    cmdTimeoutChangeLength(timeoutDuration);
}

void storeHeader(uint8_t *bfr, uint16_t atnCode, uint32_t txLen)
{
    storeDword(bfr, 0xc050d1c5); //  0..3: 0xc050d1c5 [COSmODICS] (4 bytes)
    storeWord(bfr + 4, atnCode); //  4..5: ATN code (2 bytes)
    storeDword(bfr + 6, txLen);  //  6..9: txLen (4 bytes)
}

void saveSettings(void)
{
    multicore_lockout_start_blocking();

    settings.magic = SETTINGS_MAGIC;

    uint8_t* pSettings = (uint8_t*) &settings;
    for(int i=0; i<sizeof(settings); i++) {
        EEPROM.write(i, pSettings[i]);
    }

    EEPROM.commit();

    multicore_lockout_end_blocking();
}

// Create MAC address from the flash unique ID - different for each board, but the same after EEPROM gets erased.
// (random() can't be used, it's not seeded and would give all the devices the same MAC.)
void makeMacFromBoardId(uint8_t *mac)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);  // cached at boot, so safe to call from any core without flash access

    memset(mac, 0, 6);
    for(int i=0; i<PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {     // fold 8 bytes of ID into 6 bytes of MAC
        mac[i % 6] ^= id.id[i];
    }

    mac[0] = (mac[0] & 0xFC) | 0x02;    // locally administered + unicast
}

void loadSettings(void)
{
    EEPROM.begin(512);

    // read settings from eeprom
    uint8_t* pSettings = (uint8_t*) &settings;
    for(int i=0; i<sizeof(settings); i++) {
        pSettings[i] = EEPROM.read(i);
    }

    // if settings are not stored yet, store default settings
    if(settings.magic != SETTINGS_MAGIC) {
        settings.magic = SETTINGS_MAGIC;
        settings.enabledIDs = 1;

        makeMacFromBoardId(settings.mac);

        saveSettings();
    }
}

void debugInit(void)
{
    mutex_init(&debugMutex);

#ifdef LOG_UART
    // uart0 for debug strings
    Serial1.begin(115200);
#endif

#ifdef LOG_LED
    uint offset;

    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(&uart_tx_program, &pioUartTx, &smUartTx, &offset, PIN_LED_EVB, 1, true);
    if(!success) { debug("Failed to claim PIO SM for UART\n"); while(1); }

    uart_tx_program_init(pioUartTx, smUartTx, offset, PIN_LED_EVB, 115200);

    queue_init(&fifoDebug, 1, 1024);
#endif
}

#ifdef LOG_LED
void debugFromQueue(void)
{
    while(!queue_is_empty(&fifoDebug)) {
        char c;
        queue_try_remove(&fifoDebug, &c);
        uart_tx_program_putc(pioUartTx, smUartTx, c);
    }
}
#endif

void debug(const char *fmt, ...)
{
    // Not from IRQ handlers - if the interrupted code on this core holds debugMutex, we would wait for it forever.
    if(__get_current_exception() != 0) {
        return;
    }

    mutex_enter_blocking(&debugMutex);

    // static - too big for the core stacks (core 1 has just 2 kB), and debugMutex makes sure only one caller uses them
    static char buf[1024];
    static char buf2[2 * sizeof(buf)];      // each char might get extra \r

    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if(len < 0) {                           // encoding error, nothing to show
        mutex_exit(&debugMutex);
        return;
    }

    if(len >= (int) sizeof(buf)) {          // output was truncated, vsnprintf returns the length it wanted to write
        len = sizeof(buf) - 1;
    }

    // replace \n with \n\r
    int j=0;
    for(int i=0; i<len; i++) {
        buf2[j++] = buf[i];

        if(buf[i] == '\n') {
            buf2[j++] = '\r';
        }
    }
    buf2[j] = 0;

#ifdef LOG_UART
    // use uart0 for debug strings
    uart_puts(uart0, buf2);
#endif

#ifdef LOG_LED
    if(get_core_num() == 1)         // core 1 - just add to fifo
    {
        int i = 0;

        while(buf2[i] != 0) {
            queue_try_add(&fifoDebug, &buf2[i]);
            i++;
        }
    }
    else                            // core 0 - actually send to uart
    {
        uart_tx_program_puts(pioUartTx, smUartTx, buf2);
    }
#endif

    mutex_exit(&debugMutex);
}
