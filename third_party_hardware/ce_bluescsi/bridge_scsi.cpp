// SCSI target on the BlueSCSI v2 board (the "bridge" between the SCSI bus and the rest of the firmware).
//
// Written for this firmware (not taken from the BlueSCSI firmware), only the pinout and the electrical behaviour of the
// board come from the BlueSCSI v2 schematic (hardware/Desktop_50_Pin), see board.h:
//
// - All control outputs (BSY, C/D, I/O, MSG, REQ) go through 74LVT125 buffers enabled by our BSY output: nothing gets
//   to the bus till we assert BSY, and releasing BSY releases all of them. So they get set H (inactive) before BSY.
// - C/D and MSG share the GPIO with the SEL and BSY inputs (through 1k): while we don't hold BSY, those GPIOs are inputs
//   and read SEL / BSY, while we hold BSY they are outputs. SEL can't be watched after we answered the selection - the
//   same as in the BlueSCSI firmware, the initiator releases it once it sees our BSY.
// - ATN and ACK can be read only while we hold BSY, RST any time.
// - The data bus goes through 74LVT245s, DATA_DIR H = we drive the bus. The data is inverted on the GPIOs.

#include <stdio.h>
#include "pico/stdlib.h"

#include "defs.h"
#include "board.h"
#include "bridge.h"
#include "utils.h"
#include "led.h"

#include "scsi_write.pio.h"
#include "scsi_read.pio.h"

extern uint8_t brStat; // status from bridge

static PIO pioScsiWrite, pioScsiRead;
static uint smScsiWrite, smScsiRead;
static uint offsetScsiWrite, offsetScsiRead;

uint16_t byteWithParity[256];

static uint32_t maskReq, maskCd, maskMsg, maskIo, maskRst, maskPhase;

static uint32_t lastBlockDone;          // bytes of the last DMA_readBlock() put in the FIFO - for busStateString()

static bool scsiListening = false;      // false until the end of core 1 setup - selections are ignored till then
static bool busy = false;               // we hold BSY

// prefill the table with bytes inverted, extended with parity
static void prefillBytesWithParityTable(void)
{
    for(int i=0; i<256; i++) {
        uint16_t valPar = i;
        uint8_t parity = __builtin_parity(i);

        if(!parity) {           // if odd parity, then set parity bit
            valPar |= 0x100;
        }

        byteWithParity[i] = (~valPar) & 0x1ff;
    }
}

static void pinIn(uint pin, bool pullUp)
{
    gpio_init(pin);             // SIO, input, output value L
    gpio_set_dir(pin, GPIO_IN);

    if(pullUp) {
        gpio_pull_up(pin);
    } else {
        gpio_disable_pulls(pin);
    }
}

static void pinOut(uint pin, bool level)
{
    gpio_init(pin);
    gpio_put(pin, level);       // the level first, so there's no glitch when it becomes an output
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
}

// All the pins to the idle state - bus released, watching SEL / BSY / RST.
static void pinsInit(void)
{
    maskReq = 1u << board.pinReq;
    maskCd  = 1u << board.pinCdSel;
    maskMsg = 1u << board.pinMsgBsy;
    maskIo  = 1u << board.pinIo;
    maskRst = 1u << board.pinRst;
    maskPhase = maskCd | maskMsg | maskIo;

    pinOut(board.pinBsy, 1);            // first - BSY H keeps all the control outputs off the bus
    pinOut(PIN_DATA_DIR, 0);            // data bus -> pico

    for(int i = PIN_D0; i <= PIN_DBP; i++) {
        pinIn(i, true);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);   // for later, when they become outputs
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
    }

    pinOut(board.pinReq, 1);
    pinOut(board.pinIo, 1);

    pinIn(board.pinCdSel, false);       // SEL through 1k from an always enabled buffer
    gpio_put(board.pinCdSel, 1);        // C/D inactive, once it becomes an output
    gpio_set_drive_strength(board.pinCdSel, GPIO_DRIVE_STRENGTH_12MA);

    pinIn(board.pinMsgBsy, false);      // BSY through 1k from an always enabled buffer
    gpio_put(board.pinMsgBsy, 1);       // MSG inactive, once it becomes an output
    gpio_set_drive_strength(board.pinMsgBsy, GPIO_DRIVE_STRENGTH_12MA);

    pinIn(board.pinRst, true);          // L only while RST is asserted (+ 4.7k pull-up on the board)
    pinIn(PIN_ACK, true);               // not driven while we don't hold BSY
    pinIn(board.pinAtn, true);          // the same

    busy = false;
}

// we got selected - drive the phase signals (inactive), then BSY, which enables our bus drivers
static void assertBusy(void)
{
    if(busy) {
        return;
    }

    sio_hw->gpio_set = maskPhase | maskReq;     // C/D, MSG, I/O, REQ inactive
    sio_hw->gpio_oe_set = maskCd | maskMsg;     // C/D and MSG pins become outputs - SEL / BSY can't be read now

    gpio_put(board.pinBsy, 0);                  // BSY asserted, the buffers are on now
    busy = true;

    busy_wait_us(1);                            // ATN gets readable through the buffer enabled by BSY
}

// release everything, BSY last - the bus is free then
static void releaseBus(void)
{
    sio_hw->gpio_oe_clr = SCSI_DATA_PINS_MASK;  // data pins as inputs
    gpio_put(PIN_DATA_DIR, 0);                  // data bus -> pico

    sio_hw->gpio_set = maskPhase | maskReq;     // C/D, MSG, I/O, REQ inactive (SIO level)
    sio_hw->gpio_oe_set = maskReq | maskIo;

    for(int i = PIN_D0; i <= PIN_DBP; i++) {    // data and REQ back from PIO to SIO
        gpio_set_function(i, GPIO_FUNC_SIO);
    }
    gpio_set_function(board.pinReq, GPIO_FUNC_SIO);

    if(busy) {
        gpio_put(board.pinBsy, 1);              // BSY released - all our control outputs are off the bus now
        busy = false;
        busy_wait_us(1);
    }

    sio_hw->gpio_oe_clr = maskCd | maskMsg;     // read SEL / BSY again
}

static void setScsiPhase(int newPhase)
{
    static int currentPhase = MODE_UNKNOWN;

    assertBusy();           // should be already - in case it isn't, the phase lines wouldn't get to the bus

    // asserted = L
    uint32_t asserted = 0;
    switch(newPhase)
    {
        case MODE_CMD:              asserted = maskCd;                       break;
        case MODE_MSG_OUT:          asserted = maskCd | maskMsg;             break;
        case MODE_DMA_READ:         asserted = maskIo;                       break;
        case MODE_DMA_WRITE:        asserted = 0;                            break;
        case MODE_STATUS:           asserted = maskCd | maskIo;              break;
        case MODE_MSG_IN:           asserted = maskCd | maskMsg | maskIo;    break;
    }

    sio_hw->gpio_set = maskPhase & ~asserted;
    sio_hw->gpio_clr = asserted;

    if(newPhase != currentPhase) {
        currentPhase = newPhase;
        busy_wait_us(1);    // bus settle delay (400 ns) after the phase change, before the first REQ
    }
}

static void setDataDirection(uint8_t sendNotRecv, PIO pio, uint sm)
{
    if (sendNotRecv == DIR_SEND) {  // transceivers to bus direction before the pins become outputs
        gpio_put(PIN_DATA_DIR, 1);
    }

    if(sendNotRecv == DIR_SEND) {   // outputs?
        sio_hw->gpio_oe_set = SCSI_DATA_PINS_MASK;
    } else {                        // inputs!
        sio_hw->gpio_oe_clr = SCSI_DATA_PINS_MASK;
    }

    if(pio != NULL) {
        uint32_t dirs = (sendNotRecv == DIR_SEND) ? (SCSI_DATA_PINS_MASK | maskReq) : maskReq;
        pio_sm_set_pins_with_mask(pio, sm, maskReq, maskReq);           // REQ H
        pio_sm_set_pindirs_with_mask(pio, sm, dirs, SCSI_DATA_PINS_MASK | maskReq);
    }

    for (int i = PIN_D0; i <= PIN_DBP; i++) {
        if(pio != NULL) {           // got pio, pin handled by PIO
            pio_gpio_init(pio, i);
        } else {                    // no pio, pin handled by SIO
            gpio_set_function(i, GPIO_FUNC_SIO);
        }
    }

    if(pio != NULL) {
        pio_gpio_init(pio, board.pinReq);
    } else {
        gpio_set_function(board.pinReq, GPIO_FUNC_SIO);
    }

    if (sendNotRecv == DIR_RECV) {  // transceivers to pico direction after the pins became inputs
        gpio_put(PIN_DATA_DIR, 0);
    }
}

void pioConfigAll(void)
{
    pinsInit();

    // gpio range 0..28 - all the SCSI pins, the PIO uses GPIO base 0 then (RP2350)
    bool success;

    success = pio_claim_free_sm_and_add_program_for_gpio_range(&scsi_write_program, &pioScsiWrite, &smScsiWrite, &offsetScsiWrite, 0, 29, true);
    if(!success) { debug("Failed to claim PIO SM 1\n"); haltWithError(LED_ERR_PIO); }
    scsi_write_program_init(pioScsiWrite, smScsiWrite, offsetScsiWrite, board.pinReq);

    success = pio_claim_free_sm_and_add_program_for_gpio_range(&scsi_read_program, &pioScsiRead, &smScsiRead, &offsetScsiRead, 0, 29, true);
    if(!success) { debug("Failed to claim PIO SM 2\n"); haltWithError(LED_ERR_PIO); }
    scsi_read_program_init(pioScsiRead, smScsiRead, offsetScsiRead, board.pinReq);

    prefillBytesWithParityTable();

    debug("bridge: %s, REQ GP%d, C/D+SEL GP%d, MSG+BSY GP%d, RST GP%d, I/O GP%d, BSY GP%d, ATN GP%d\n", board.name,
          board.pinReq, board.pinCdSel, board.pinMsgBsy, board.pinRst, board.pinIo, board.pinBsy, board.pinAtn);
}

int pioConfig(int newMode, bool force)
{
    static int currentMode = MODE_UNKNOWN;
    int oldMode = currentMode;

    if(!force && newMode == currentMode) {    // no mode change? just quit
        return oldMode;
    }
    currentMode = newMode;

    // all state machines to halt
    pio_sm_set_enabled(pioScsiWrite, smScsiWrite, false);
    pio_sm_set_enabled(pioScsiRead, smScsiRead, false);

    PIO whichPio = NULL;
    uint whichSm = 0;
    uint whichOffset = 0;

    switch(newMode)
    {
        // write modes - bytes from the initiator
        case MODE_CMD:
        case MODE_DMA_WRITE:
        case MODE_MSG_OUT:
            whichPio = pioScsiWrite;
            whichSm = smScsiWrite;
            whichOffset = offsetScsiWrite;
            break;

        // read modes - bytes to the initiator
        case MODE_DMA_READ:
        case MODE_STATUS:
        case MODE_MSG_IN:
            whichPio = pioScsiRead;
            whichSm = smScsiRead;
            whichOffset = offsetScsiRead;
            break;

        default:            // selection / reset / unknown - bus released, both state machines halted
            if(newMode != MODE_SCSI_SELECTION) {
                debug("pioConfig - unknown mode %d\n", newMode);
                currentMode = MODE_SCSI_SELECTION;
            }
            releaseBus();
            return oldMode;
    }

    setScsiPhase(newMode);

    // data direction RECV for CMD and WRITE, data direction SEND for READ and STATUS
    uint8_t sendNotRecv = (newMode == MODE_DMA_READ || newMode == MODE_STATUS || newMode == MODE_MSG_IN) ? DIR_SEND : DIR_RECV;
    setDataDirection(sendNotRecv, whichPio, whichSm);

    // restart state machine, clear FIFOs, enable state machine
    pio_sm_restart(whichPio, whichSm);
    pio_sm_clear_fifos(whichPio, whichSm);

    // pio_sm_restart() doesn't touch PC and X - after an aborted transfer (timeout) the program would continue where it stopped,
    // e.g. scsi_write in wait4L with REQ L and the old byte count. So jump to the start, with side-set 1 to keep REQ released.
    pio_sm_exec(whichPio, whichSm, pio_encode_jmp(whichOffset) | pio_encode_sideset(1, 1));     // .side_set 1 in scsi_write / scsi_read

    pio_sm_set_enabled(whichPio, whichSm, true);

    return oldMode;
}

void busStartListening(void)
{
    scsiListening = true;
}

bool isBusResetAsserted(void)
{
    return (sio_hw->gpio_in & maskRst) == 0;
}

bool isAtnAsserted(void)
{
    if(!busy) {             // ATN gets to the pico only while we hold BSY
        return false;
    }

    return BIT_IS_L(board.pinAtn);
}

uint8_t isSelectionHappening(void)
{
    if(!scsiListening) {
        return false;
    }

    pioConfig(MODE_SCSI_SELECTION, busy);   // still holding BSY here (e.g. selected, then the command failed)? release it

    if(isBusResetAsserted()) {              // no selection while RST is L
        return false;
    }

    return BIT_IS_L(board.pinCdSel) && BIT_IS_H(board.pinMsgBsy);     // SEL asserted, BSY released by the initiator
}

// Called after isSelectionHappening(). Waits while SEL is asserted, and when one of our IDs is selected, answers with BSY.
uint8_t getSelectionByte(void)
{
    timeoutStart();             // start the timeout timer
    brStat = E_OK;              // init bridge status to E_OK

    uint8_t prevIds = 0;

    while(BIT_IS_L(board.pinCdSel) && !hasTimedOut)     // while SEL pin is asserted and it's not timeout yet
    {
        if(BIT_IS_L(board.pinMsgBsy) || isBusResetAsserted()) {     // BSY asserted again (arbitration / another target answered) or reset
            prevIds = 0;
            continue;
        }

        uint8_t selectedIds = ((uint8_t) ~(gpio_get_all()));    // get all data bits
        uint8_t stableIds = selectedIds & prevIds;              // the same in 2 reads in a row - no glitch
        prevIds = selectedIds;

        for (int i = 0; i < 8; i++)
        {
            int bit = (1 << i);

            if ((stableIds & bit) && (settings.enabledIDs & bit))    // if the bit #i set and is this ID enabled?
            {
                assertBusy();   // answer the selection - the initiator releases SEL then
                return i;       // return this ID
            }
        }
    }

    return 0xff;            // this device was not selected
}

// get next CMD / MSG OUT byte from the initiator
uint8_t PIO_write(void)
{
    pio_sm_put(pioScsiWrite, smScsiWrite, 0);      // write N-1 count of bytes to write here

    while(1)
    {
        if(hasTimedOut || isBusResetAsserted()) {   // on timeout / bus reset
            brStat = E_TimeOut; // set the bridge status
            return 0;
        }

        // on rx fifo has data
        if(!pio_sm_is_rx_fifo_empty(pioScsiWrite, smScsiWrite)) {
            break;
        }
    }

    uint8_t val = ((uint8_t) (pio_sm_get(pioScsiWrite, smScsiWrite) >> 24));
    val = ~val;     // invert data
    return val;
}

// The read SM is done when it waits on the pull (PC at its start) with nothing in the TX FIFO. Checked twice, more than
// 1 PIO cycle apart - a pull which just took the last byte moves the PC on in the next cycle. (Just checking the FIFO and
// the REQ / ACK levels isn't enough: right after the pull of the last byte both are H for a moment.)
static bool __not_in_flash_func(readSmIdle)(void)
{
    PIO pio = pioScsiRead;
    const uint32_t txEmpty = 1u << (PIO_FSTAT_TXEMPTY_LSB + smScsiRead);

    if(!(pio->fstat & txEmpty) || pio->sm[smScsiRead].addr != offsetScsiRead) {
        return false;
    }

    busy_wait_at_least_cycles(20);      // > 20 ns at up to 150 MHz + PIO at 50 MHz

    return (pio->fstat & txEmpty) && pio->sm[smScsiRead].addr == offsetScsiRead;
}

static void PIO_read(uint8_t val)
{
    uint32_t valPar = byteWithParity[val];
    pio_sm_put_blocking(pioScsiRead, smScsiRead, valPar);     // write status byte to TX FIFO, the transfer will start

    // wait for data to be transfered
    while(1)
    {
        if(hasTimedOut || isBusResetAsserted()) {   // on timeout / bus reset
            brStat = E_TimeOut; // set the bridge status
            return;
        }

        if(readSmIdle()) {
            break;
        }
    }

    brStat = E_OK;
}

// send status byte to the initiator, followed by MSG IN (COMMAND COMPLETE), then release the bus
void statusAndMsgRead(uint8_t scsiStatusByte)
{
    // if we didn't have bridge timeout, we can try to send STATUS byte
    if (brStat != E_TimeOut)
    {
        pioConfig(MODE_STATUS, false);
        PIO_read(scsiStatusByte);

        if(brStat == E_TimeOut) {
            debug("TO on STATUS\n");
        }
    }

    // if we didn't have bridge timeout, we can try to send MSG IN
    if (brStat != E_TimeOut)
    {
        pioConfig(MODE_MSG_IN, false);
        PIO_read(0);

        if(brStat == E_TimeOut) {
            debug("TO on MSG_IN\n");
        }
    }

    resetBridge();           // release BSY, C/D, I/O, MSG - bus free
}

void DMA_read_waitForEnd(void)
{
    // wait while read is still in progress (from FIFO to Atari)
    while(1) {
        if(readSmIdle()) {
            return;
        }

        if(hasTimedOut || isBusResetAsserted()) {   // on timeout / bus reset
            debug("DMA_read_waitForEnd T/O\n");
            brStat = E_TimeOut; // set the bridge status
            return;
        }
    }
}

void DMA_read(uint8_t val)
{
    uint32_t valPar = byteWithParity[val];

    // wait for TX fifo not full, so we can put the current value in
    while(1) {
        // READ TX FIFO not full, we can push to fifo
        if(!pio_sm_is_tx_fifo_full(pioScsiRead, smScsiRead)) {
           pio_sm_put(pioScsiRead, smScsiRead, valPar);
           return;
        }

        if(hasTimedOut || isBusResetAsserted()) {   // on timeout / bus reset
            debug("DMA_read T/O\n");
            brStat = E_TimeOut; // set the bridge status
            return;
        }
    }
}

// Like DMA_read() for each byte, but in one loop running from RAM
void __not_in_flash_func(DMA_readBlock)(const uint8_t* bfr, uint32_t cnt)
{
    // FIFO registers directly - the pio_sm_*() inline functions might end up as calls to flash with opt=Small
    PIO pio = pioScsiRead;
    io_wo_32 *txf = &pio->txf[smScsiRead];
    const uint32_t txFull = 1u << (PIO_FSTAT_TXFULL_LSB + smScsiRead);

    uint32_t i = 0;

    while(i < cnt) {
        // TX FIFO not full, we can push to fifo
        if(!(pio->fstat & txFull)) {
            *txf = byteWithParity[bfr[i]];
            i++;
            continue;
        }

        if(hasTimedOut || (sio_hw->gpio_in & maskRst) == 0) {      // on timeout / bus reset
            brStat = E_TimeOut; // set the bridge status
            lastBlockDone = i;
            return;
        }
    }

    lastBlockDone = i;
}

void DMA_write_startWithCount(uint32_t transfersCount)
{
    if(transfersCount == 0) {   // PIO program would get 0xffffffff and wait for 4 G bytes with REQ active
        return;
    }

    pio_sm_put(pioScsiWrite, smScsiWrite, transfersCount - 1);        // write N-1 count of bytes to write here
}

uint8_t DMA_write(void)
{
    while(1)
    {
        if(hasTimedOut || isBusResetAsserted()) {   // on timeout / bus reset
            brStat = E_TimeOut; // set the bridge status
            return 0;
        }

        // on rx fifo has data
        if(!pio_sm_is_rx_fifo_empty(pioScsiWrite, smScsiWrite)) {
            break;
        }
    }

    uint8_t val = ((uint8_t) (pio_sm_get(pioScsiWrite, smScsiWrite) >> 24));
    val = ~val;
    return val;
}

// Like DMA_write() for each byte, but in one loop running from RAM
void __not_in_flash_func(DMA_writeBlock)(uint8_t* bfr, uint32_t cnt)
{
    PIO pio = pioScsiWrite;
    io_ro_32 *rxf = &pio->rxf[smScsiWrite];
    const uint32_t rxEmpty = 1u << (PIO_FSTAT_RXEMPTY_LSB + smScsiWrite);

    for(uint32_t i = 0; i < cnt; i++) {
        while(pio->fstat & rxEmpty) {   // wait for byte from ST
            if(hasTimedOut || (sio_hw->gpio_in & maskRst) == 0) {   // on timeout / bus reset
                brStat = E_TimeOut;     // set the bridge status
                return;
            }
        }

        bfr[i] = ~((uint8_t) (*rxf >> 24));     // invert data
    }
}

// Bus and read-SM state for the logs on a timeout - call it before resetBridge(), ACK / ATN read L/H only while we hold BSY.
const char* busStateString(void)
{
    static char str[128];
    uint32_t in = sio_hw->gpio_in;
    PIO pio = pioScsiRead;

    snprintf(str, sizeof(str), "BSY %s, REQ %c, ACK %c, ATN %c, RST %c, %u B to FIFO, FIFO %u, SM pc %u",
             busy ? "ours" : "free", (in & maskReq) ? 'H' : 'L', (in & (1u << PIN_ACK)) ? 'H' : 'L',
             (in & (1u << board.pinAtn)) ? 'H' : 'L', (in & maskRst) ? 'H' : 'L', lastBlockDone,
             pio_sm_get_tx_fifo_level(pio, smScsiRead), pio->sm[smScsiRead].addr - offsetScsiRead);
    return str;
}

void resetBridge(void)
{
    pioConfig(MODE_SCSI_SELECTION, true);
    brStat = E_OK; // set bridge status to OK
}
