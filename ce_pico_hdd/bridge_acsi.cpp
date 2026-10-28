// ACSI variant of the bridge (see bridge.cpp for the common functions calling these)

#include "defs.h"
#include "bridge.h"
#include "utils.h"
#include "neopixel.h"

#include "acsi_first.pio.h"
#include "acsi_write.pio.h"
#include "acsi_read.pio.h"

extern uint8_t brStat; // status from bridge

// Each program runs on one state machine, the handshake pins are switched by the config: command bytes and status
// use CS + INT, data uses ACK + DRQ. (Separate state machines for these wouldn't fit in the 8 SMs together with IKBD.)
static PIO pioAcsiFirst, pioAcsiWrite, pioAcsiRead;
static uint smAcsiFirst, smAcsiWrite, smAcsiRead;
static pio_sm_config cfgAcsiCmdWrite, cfgAcsiDataWrite, cfgAcsiDataRead, cfgAcsiStatusRead;
static uint offsetAcsiWrite, offsetAcsiRead;

int readInProgressCount = 0;

static void setDataDirection(uint8_t sendNotRecv, PIO pio, uint sm)
{
    static uint8_t sendNotRecvNow = 0xff; // init with no data direction set yet
    static PIO pioNow = nullptr;

    if (sendNotRecvNow == sendNotRecv && pioNow == pio) { // direction not changed since last time? quit
        return;
    }

    sendNotRecvNow = sendNotRecv; // remember what we're just setting
    pioNow = pio;

    // if output from mcu, set DATA_DIR to H before switching mcu pins directions
    if (sendNotRecv == DIR_SEND) {
        BIT_SET(PIN_DATA_DIR);
    }

    if(sendNotRecv == DIR_SEND) {   // outputs?
        gpio_set_dir_out_masked(DATA_PINS_MASK);
    } else {                        // inputs!
        gpio_set_dir_in_masked(DATA_PINS_MASK);
    }

    // pin directions in PIO are per PIO block, not per SM - all the ACSI SMs share one block, so the data pins set as
    // outputs for READ / STATUS would stay outputs for CMD / WRITE too, fighting IC2/IC3 while they drive the data from ST
    pio_sm_set_pindirs_with_mask64(pio, sm, (sendNotRecv == DIR_SEND) ? DATA_PINS_MASK : 0, DATA_PINS_MASK);

    int pio_data_pins[8] = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7};
    for (int i = 0; i < 8; i++) {
        pio_gpio_init(pio, pio_data_pins[i]);
    }

    // if input to mcu, set DATA_DIR to L after switching mcu pins directions
    if (sendNotRecv == DIR_RECV) {
        BIT_CLR(PIN_DATA_DIR);
    }
}

// Like pio_sm_init() with a config, but its JMP to the program start is executed with side-set 1. pio_sm_init() executes it
// with side-set 0, which pulses the handshake pin (INT / DRQ, already switched to PIO) L before the program runs - then
// the ST takes a byte too early, e.g. a garbage 1st byte of DMA read, and the rest of the data is shifted by one.
static void smInitWithHandshakeH(PIO pio, uint sm, uint offset, const pio_sm_config *cfg)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_set_config(pio, sm, cfg);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_clkdiv_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset) | pio_encode_sideset(1, 1));     // .side_set 1 in acsi_write / acsi_read
}

void acsiPioConfigAll(void)
{
    bool success;
    uint offset;

    success = pio_claim_free_sm_and_add_program_for_gpio_range(&acsi_first_program, &pioAcsiFirst, &smAcsiFirst, &offset, PIN_D0, 26, true);
    if(!success) { debug("Failed to claim PIO SM 1\n"); haltWithError(LED_ERR_PIO); }
    acsi_first_program_init(pioAcsiFirst, smAcsiFirst, offset);

    success = pio_claim_free_sm_and_add_program_for_gpio_range(&acsi_write_program, &pioAcsiWrite, &smAcsiWrite, &offsetAcsiWrite, PIN_D0, 26, true);
    if(!success) { debug("Failed to claim PIO SM 2\n"); haltWithError(LED_ERR_PIO); }
    acsi_write_program_init(pioAcsiWrite, smAcsiWrite, offsetAcsiWrite, PIN_CS, PIN_INT);
    cfgAcsiCmdWrite  = acsi_write_program_get_config(offsetAcsiWrite, PIN_CS, PIN_INT);
    cfgAcsiDataWrite = acsi_write_program_get_config(offsetAcsiWrite, PIN_ACK, PIN_DRQ);

    success = pio_claim_free_sm_and_add_program_for_gpio_range(&acsi_read_program, &pioAcsiRead, &smAcsiRead, &offsetAcsiRead, PIN_D0, 26, true);
    if(!success) { debug("Failed to claim PIO SM 3\n"); haltWithError(LED_ERR_PIO); }
    acsi_read_program_init(pioAcsiRead, smAcsiRead, offsetAcsiRead, PIN_ACK, PIN_DRQ);
    cfgAcsiDataRead   = acsi_read_program_get_config(offsetAcsiRead, PIN_ACK, PIN_DRQ);
    cfgAcsiStatusRead = acsi_read_program_get_config(offsetAcsiRead, PIN_CS, PIN_INT);
}

int acsiPioConfig(int newMode, bool force)
{
    static int currentMode = MODE_UNKNOWN;
    int oldMode = currentMode;

    if(!force && newMode == currentMode) {    // no mode change? just quit
        return oldMode;
    }
    currentMode = newMode;

    // all state machines to halt
    pio_sm_set_enabled(pioAcsiFirst, smAcsiFirst, false);
    pio_sm_set_enabled(pioAcsiWrite, smAcsiWrite, false);
    pio_sm_set_enabled(pioAcsiRead, smAcsiRead, false);

    gpio_set_function(PIN_DRQ, GPIO_FUNC_SIO);    // DRQ is controlled by gpio, always driving H
    gpio_put(PIN_DRQ, 1);

    gpio_set_function(PIN_INT, GPIO_FUNC_SIO);    // INT is controlled by gpio, always driving H
    gpio_put(PIN_INT, 1);

    pio_sm_set_pins_with_mask(pioAcsiWrite, smAcsiWrite, ACSI_HANDSHAKE_OUT_PINS, ACSI_HANDSHAKE_OUT_PINS);   // INT and DRQ to H in PIO SM output
    pio_sm_set_pins_with_mask(pioAcsiRead, smAcsiRead, ACSI_HANDSHAKE_OUT_PINS, ACSI_HANDSHAKE_OUT_PINS);     // INT and DRQ to H in PIO SM output

    PIO whichPio;
    uint whichSm;
    pio_sm_config *whichCfg = NULL;     // for the shared state machines - which handshake pins to use
    uint whichOffset = 0;

    switch(newMode)
    {
        case MODE_RESET:
        case MODE_ACSI_FIRST:
            configAcsiFirst();

            whichPio = pioAcsiFirst;
            whichSm = smAcsiFirst;
            break;

        case MODE_CMD_REST:
            pio_gpio_init(pioAcsiWrite, PIN_INT);   // INT is controlled by PIO

            whichPio = pioAcsiWrite;
            whichSm = smAcsiWrite;
            whichCfg = &cfgAcsiCmdWrite;
            whichOffset = offsetAcsiWrite;
            break;

        case MODE_DMA_READ:
            pio_gpio_init(pioAcsiRead, PIN_DRQ);    // DRQ is controlled by PIO
            pio_sm_set_pindirs_with_mask64(pioAcsiRead, smAcsiRead, DATA_PINS_MASK, DATA_PINS_MASK);

            readInProgressCount = 0;        // no read bytes in progress

            whichPio = pioAcsiRead;
            whichSm = smAcsiRead;
            whichCfg = &cfgAcsiDataRead;
            whichOffset = offsetAcsiRead;
            break;

        case MODE_DMA_WRITE:
            pio_gpio_init(pioAcsiWrite, PIN_DRQ);   // DRQ is controlled by PIO

            whichPio = pioAcsiWrite;
            whichSm = smAcsiWrite;
            whichCfg = &cfgAcsiDataWrite;
            whichOffset = offsetAcsiWrite;
            break;

        case MODE_STATUS:
            pio_gpio_init(pioAcsiRead, PIN_INT);    // INT is controlled by PIO
            pio_sm_set_pindirs_with_mask64(pioAcsiRead, smAcsiRead, DATA_PINS_MASK, DATA_PINS_MASK);

            whichPio = pioAcsiRead;
            whichSm = smAcsiRead;
            whichCfg = &cfgAcsiStatusRead;
            whichOffset = offsetAcsiRead;
            break;

        default:            // unknown mode - leave all state machines halted, INT and DRQ H
            debug("pioConfig - unknown mode %d\n", newMode);
            return oldMode;
    }

    // data direction RECV for CMD and WRITE, data direction SEND for READ and STATUS
    uint8_t sendNotRecv = (newMode == MODE_DMA_READ || newMode == MODE_STATUS) ? DIR_SEND : DIR_RECV;
    setDataDirection(sendNotRecv, whichPio, whichSm);

    // if we're in the reset mode, don't enabble any PIO SM
    if(newMode == MODE_RESET) {
        return oldMode;
    }

    if(whichCfg) {      // shared state machine - load the config with the handshake pins for this mode, start from the beginning, clear FIFOs
        smInitWithHandshakeH(whichPio, whichSm, whichOffset, whichCfg);
    } else {            // restart state machine, clear FIFOs
        pio_sm_restart(whichPio, whichSm);
        pio_sm_clear_fifos(whichPio, whichSm);
    }

    pio_sm_set_enabled(whichPio, whichSm, true);
    return oldMode;
}

uint8_t PIO_gotFirstCmdByte(void)
{
    if(BIT_IS_L(PIN_RESET)) {   // no new cmd bytes when ACSI RESET is L
        return false;
    }

    acsiPioConfig(MODE_ACSI_FIRST, false);
    return !pio_sm_is_rx_fifo_empty(pioAcsiFirst, smAcsiFirst);
}

// get 1st CMD byte from ST  -- without setting INT
uint8_t PIO_writeFirst(void)
{
    timeoutStart();             // start the timeout timer

    brStat = E_OK;              // init bridge status to E_OK
    return ((uint8_t) (pio_sm_get(pioAcsiFirst, smAcsiFirst) >> 24));
}

// get next CMD byte from ST -- with setting INT to LOW and waiting for CS
uint8_t acsiPIO_write(void)
{
    acsiPioConfig(MODE_CMD_REST, false);
    pio_sm_put(pioAcsiWrite, smAcsiWrite, 0);      // write N-1 count of bytes to write here

    while(1)
    {
        if(hasTimedOut) {       // on timeout
            brStat = E_TimeOut; // set the bridge status
            return 0;
        }

        // on rx fifo has data
        if(!pio_sm_is_rx_fifo_empty(pioAcsiWrite, smAcsiWrite)) {
            break;
        }
    }

    return ((uint8_t) (pio_sm_get(pioAcsiWrite, smAcsiWrite) >> 24));
}

// send status byte to host (ACSI has no MSG IN phase)
void acsiStatusRead(uint8_t scsiStatusByte)
{
    acsiPioConfig(MODE_STATUS, false);

    pio_sm_put(pioAcsiRead, smAcsiRead, scsiStatusByte);     // write status byte to TX FIFO, the transfer will start

    // wait for data to be transfered
    while(1)
    {
        if(hasTimedOut) {       // on timeout
            brStat = E_TimeOut; // set the bridge status
            return;
        }

        // on rx fifo has data, this means that transfer has finished with success
        if(!pio_sm_is_rx_fifo_empty(pioAcsiRead, smAcsiRead)) {
            uint32_t tmp = pio_sm_get(pioAcsiRead, smAcsiRead);
            break;
        }
    }

    brStat = E_OK;
}

void acsiDMA_read_waitForEnd(void)
{
    // wait while read is still in progress (from FIFO to Atari)
    while(1) {
        if(readInProgressCount <= 0) {  // nothing in progress? this the normal end
            readInProgressCount = 0;
            return;
        }

        // RX FIFO not empty? read it, decrement readInProgressCount
        if(!pio_sm_is_rx_fifo_empty(pioAcsiRead, smAcsiRead)) {
            uint32_t tmp = pio_sm_get(pioAcsiRead, smAcsiRead);
            readInProgressCount--;
        }

        if(hasTimedOut) {       // on timeout
            brStat = E_TimeOut; // set the bridge status
            return;
        }
    }
}

void acsiDMA_read(uint8_t val)
{
    // wait for TX fifo not full, so we can put the current value in
    while(1) {
        // RX FIFO not empty? read it, decrement readInProgressCount
        if(!pio_sm_is_rx_fifo_empty(pioAcsiRead, smAcsiRead)) {
            uint32_t tmp = pio_sm_get(pioAcsiRead, smAcsiRead);
            readInProgressCount--;
        }

        // READ TX FIFO not full, we can push to fifo
        if(!pio_sm_is_tx_fifo_full(pioAcsiRead, smAcsiRead)) {
           // put current byte in TX FIFO, increment readInProgressCount
           pio_sm_put(pioAcsiRead, smAcsiRead, val);
           readInProgressCount++;
           return;
        }

        if(hasTimedOut) {       // on timeout
            brStat = E_TimeOut; // set the bridge status
            return;
        }
    }
}

// Like acsiDMA_read() for each byte, but in one loop running from RAM - the call per byte (through bridge.cpp) and flash
// cache misses (the XIP cache is shared with core 0) made the CPU too slow to keep the TX FIFO from running empty.
void __not_in_flash_func(acsiDMA_readBlock)(const uint8_t* bfr, uint32_t cnt)
{
    // FIFO registers directly - the pio_sm_*() inline functions might end up as calls to flash with opt=Small
    PIO pio = pioAcsiRead;
    io_ro_32 *rxf = &pio->rxf[smAcsiRead];
    io_wo_32 *txf = &pio->txf[smAcsiRead];
    const uint32_t rxEmpty = 1u << (PIO_FSTAT_RXEMPTY_LSB + smAcsiRead);
    const uint32_t txFull  = 1u << (PIO_FSTAT_TXFULL_LSB + smAcsiRead);

    int inProgress = readInProgressCount;
    uint32_t i = 0;

    while(i < cnt) {
        uint32_t fstat = pio->fstat;

        // RX FIFO not empty? read it, decrement inProgress
        if(!(fstat & rxEmpty)) {
            (void) *rxf;
            inProgress--;
        }

        // TX FIFO not full, we can push to fifo
        if(!(fstat & txFull)) {
            *txf = bfr[i];
            i++;
            inProgress++;
            continue;
        }

        if(hasTimedOut) {       // on timeout
            brStat = E_TimeOut; // set the bridge status
            break;
        }
    }

    readInProgressCount = inProgress;
}

void acsiDMA_write_startWithCount(uint32_t transfersCount)
{
    if(transfersCount == 0) {   // PIO program would get 0xffffffff and wait for 4 G bytes with DRQ / REQ active
        return;
    }

    pio_sm_put(pioAcsiWrite, smAcsiWrite, transfersCount - 1);        // write N-1 count of bytes to write here
}

uint8_t acsiDMA_write(void)
{
    while(1)
    {
        if(hasTimedOut) {       // on timeout
            brStat = E_TimeOut; // set the bridge status
            return 0;
        }

        // on rx fifo has data
        if(!pio_sm_is_rx_fifo_empty(pioAcsiWrite, smAcsiWrite)) {
            break;
        }
    }

    return ((uint8_t) (pio_sm_get(pioAcsiWrite, smAcsiWrite) >> 24));
}

// Like acsiDMA_write() for each byte, but in one loop running from RAM - see acsiDMA_readBlock()
void __not_in_flash_func(acsiDMA_writeBlock)(uint8_t* bfr, uint32_t cnt)
{
    PIO pio = pioAcsiWrite;
    io_ro_32 *rxf = &pio->rxf[smAcsiWrite];
    const uint32_t rxEmpty = 1u << (PIO_FSTAT_RXEMPTY_LSB + smAcsiWrite);

    for(uint32_t i = 0; i < cnt; i++) {
        while(pio->fstat & rxEmpty) {   // wait for byte from ST
            if(hasTimedOut) {           // on timeout
                brStat = E_TimeOut;     // set the bridge status
                return;
            }
        }

        bfr[i] = (uint8_t) (*rxf >> 24);
    }
}

void acsiResetBridge(void)
{
    acsiPioConfig(MODE_ACSI_FIRST, true);
    brStat = E_OK; // set bridge status to OK
}

// On ACSI the neopixel pin has no other function and the gate (GP12 = INT) is open while INT is not asserted,
// which is always the case between commands. The pin is an output idling L, so the gap before the frame is a reset already.
void acsiNeopixelOutput(uint32_t grb)
{
    neopixelSendFrame(grb);
}
