#include <stdio.h>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/spi.h"

#include "defs.h"
#include "utils.h"
#include "display.h"
#include "psram.h"
#include "ipc.h"
#include "mfm_write.h"
#include "core1_main.h"
#include "beeper.h"

volatile bool core1running = false;

#define MFM_BUFFER_SIZE         2048                                // 12 address bits masked for ring mode (4096 bytes == 2048 words)
#define MFM_BUFFER_HALF_SIZE    (MFM_BUFFER_SIZE / 2)
#define MFM_READ_SIZE_FILLS     (MFM_BUFFER_HALF_SIZE / 4)

__attribute__((aligned(4096))) uint16_t mfmBuffer[MFM_BUFFER_SIZE]; // must align to 4096 for ring mode to work correctly

int dmaChannel;
volatile bool halfDone = false;

#define FILL_NONE   0
#define FILL_LOWER  1
#define FILL_UPPER  2

volatile uint8_t fillWhat = FILL_NONE;

extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];
uint32_t dataIndexInTrack = STREAM_START_OFFSET;
volatile uint32_t lastStepTime = 0;
volatile uint32_t timeTrackStart;

extern TDrivePosition posStreamed, hwPosition;

void fillHalfMfmBuffer(uint8_t what);
void readTrackData_goToStart(void);

extern uint8_t imgTracks, imgSides, imgSectorsPerTrack;

volatile IPCbuffer* wrBuffer = NULL;

enum MfmStreamState {
  STATE_STREAMING,          // no step occured recently, we can just stream
  STATE_STEPPING,           // step happened less than 15 ms ago, there might be more, don't stream
  STATE_LOADING             // we're now loading data from PSRAM to SRAM, once this is done we can start streaming
};

volatile MfmStreamState state = STATE_STEPPING;

void __isr dmaHandlerMfm(void);
void __isr gpioIsrCore1(uint gpio, uint32_t events);

// interrupt handler for STEP signal
void __isr floppyStepISR(uint gpio, uint32_t event_mask)
{
    uint32_t now = millis();

    if((now - lastStepTime) < 1) {  // last step ISR in this same ms? this is a glitch, ignore it (fastest ST step rate is 2 ms, which can measure as 1 ms)
        return;
    }
    lastStepTime = now;

    if(BIT_IS_H(PIN_MOT_EN)) {       // motor not enabled? Skip the following code.
        return;
    }

    beeperStepClick();

    if(BIT_IS_H(PIN_DIR)) {  // direction is High? track--
        if(hwPosition.track > 0) {
            hwPosition.track--;
        }
    } else  {                // direction is Low? track++
        if(hwPosition.track < (MAX_TRACKS - 1)) {     // tracks 0 .. MAX_TRACKS-1
            hwPosition.track++;
        }
    }

    if(hwPosition.track == 0) {   // if track is 0, TRACK00 is L
        gpio_put(PIN_TRACK00, 0);
    } else {                        // if track is not 0, TRACK00 to H
        gpio_put(PIN_TRACK00, 1);
    }
}

void setupPwmOutput(void)
{
  gpio_set_function(PIN_RDATA, GPIO_FUNC_PWM);
  uint pwmSliceNum = pwm_gpio_to_slice_num(PIN_RDATA);
  uint pwmChannel = pwm_gpio_to_channel(PIN_RDATA);

  pwm_set_clkdiv_int_frac(pwmSliceNum, 75, 0);      // 150 MHz / 75 = 2 MHz -- 1 tick is 0.5 us
  pwm_set_output_polarity(pwmSliceNum, true, true);

  pwm_set_wrap(pwmSliceNum, 7);                     // Set period (timer going from from 0 to this value)
  pwm_set_chan_level(pwmSliceNum, pwmChannel, 1);   // Set channel output high for one cycle before dropping

  pwm_set_enabled(pwmSliceNum, true);               // Set the PWM running
}

void setupDmaToPwm(void)
{
    uint pwmChannel = pwm_gpio_to_channel(PIN_RDATA);
    uint pwmSliceNum = pwm_gpio_to_slice_num(PIN_RDATA);

    dmaChannel = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dmaChannel);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);     // Transfer size: 16 bits
    channel_config_set_read_increment(&c, true);                // Source increments through mfmBuffer
    channel_config_set_write_increment(&c, false);              // Destination is fixed (PWM CC register)
    channel_config_set_dreq(&c, pwm_get_dreq(pwmSliceNum));     // Pace by PWM slice DREQ (fires at counter wrap)
    channel_config_set_ring(&c, false, 12);                     // enable ring mode, on read side, 12 address bits masked for ring mode (4096 bytes == 2048 words)

    // dma_encode_endless_transfer_count() vs MFM_BUFFER_HALF_SIZE
    dma_channel_configure(  dmaChannel, &c,
                            (volatile void *) (&pwm_hw->slice[pwmSliceNum].top),
                            mfmBuffer,
                            dma_encode_transfer_count_with_self_trigger(MFM_BUFFER_HALF_SIZE),
                            true);

    // Enable DMA interrupt
    dma_channel_set_irq0_enabled(dmaChannel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dmaHandlerMfm);
    irq_set_enabled(DMA_IRQ_0, true);
}

// DMA IRQ handler
void __isr dmaHandlerMfm(void)
{
    // Clear the interrupt
    dma_hw->ints0 = 1u << dmaChannel;

    halfDone = !halfDone;   // Flip half flag

    fillWhat = halfDone ? FILL_LOWER : FILL_UPPER;

    if(state == STATE_STREAMING) {
        fillHalfMfmBuffer(fillWhat);
    }
}

void getMfmDataToBuffer(uint8_t* bfr, int len)
{
    // update SIDE var
    hwPosition.side = BIT_IS_H(PIN_SIDE1) ? 0 : 1; // get the current SIDE

    // get current track and side we should be streaming, limit them to maximum values
    int trackNo = MIN(hwPosition.track, MAX_TRACKS - 1);
    int sideNo = MIN(hwPosition.side, 1);

    uint8_t* pTrackDataStart = (sideNo == 0) ? trackData0 : trackData1;
    uint8_t* pTrackDataEnd = &pTrackDataStart[READTRACKDATA_SIZE_BYTES - 1];
    uint8_t* pTrackData = &pTrackDataStart[MIN(dataIndexInTrack, READTRACKDATA_SIZE_BYTES - 1)];  // copy data from here, never from beyond the track buffer

    memset(bfr, 0x55, len);                // init all values to 0x55

    for(int i=0; i<len; ) {
        // end of array or end-of-track marker? we've at the end, don't copy anything more (position checked before reading from it)
        if(pTrackData >= pTrackDataEnd || *pTrackData == CMD_TRACK_STREAM_END) {
            break;
        }

        uint8_t val = *pTrackData++;

        // skip empty bytes
        if(val == 0) {
            continue;
        }

        // skip data part of the sector marker
        if(val == CMD_DATA_PART_OF_SECTOR) {
            continue;
        }

        // current sector marker?
        if(val == CMD_CURRENT_SECTOR) {
            if(pTrackData + 3 > pTrackDataEnd) {    // mark cut off by the end of the track buffer? nothing more to stream
                pTrackData = pTrackDataEnd;
                break;
            }

            posStreamed.side = *pTrackData++;
            posStreamed.track = *pTrackData++;
            posStreamed.sector = *pTrackData++;
            continue;
        }

        // val is just mfm data, store it
        bfr[i] = val;
        i++;
    }

    // now we got 'len' bytes in the bfr, we just need to update data retrieval index
    dataIndexInTrack = pTrackData - pTrackDataStart;
}

const uint16_t arrValues[4] = {7, 7, 11, 15};       // conversion table from mfm packed symbol to timer ARR value (for 0 us, 4 us, 6 us, 8 us)

void fillHalfMfmBuffer(uint8_t what)
{
    uint16_t* bfr = (what == FILL_LOWER) ? &mfmBuffer[0] : &mfmBuffer[MFM_BUFFER_HALF_SIZE];

    // from the track buffer (with all the additional data and spaces) extract
    // just MFM_READ_SIZE_FILLS bytes which can be transformed into MFM intervals
    uint8_t rawMfmData[MFM_READ_SIZE_FILLS];
    getMfmDataToBuffer(rawMfmData, MFM_READ_SIZE_FILLS);

    for(int i=0; i<MFM_READ_SIZE_FILLS; i++) {
        // get one byte with 4 intervals
        uint8_t streamByte = rawMfmData[i];

        // unpack intervals into 4 items of uint16_t
        bfr[0] = arrValues[ ((streamByte >> 6) & 3) ];
        bfr[1] = arrValues[ ((streamByte >> 4) & 3) ];
        bfr[2] = arrValues[ ((streamByte >> 2) & 3) ];
        bfr[3] = arrValues[ ((streamByte     ) & 3) ];

        bfr += 4;
    }
}

void clearMfmBuffer(void)
{
    // copy 'all 4 us' pulses into current streaming buffer to allow shortest possible switch to start of track
    for(int i=0; i<MFM_BUFFER_SIZE; i++) {
        mfmBuffer[i] = 7;
    }
}

void readTrackData_goToStart(void)
{
    dataIndexInTrack = STREAM_START_OFFSET;     // stream index to start
    timeTrackStart = millis();                  // time of track start to now
}

// Track rotation: 200 ms per track, INDEX low for the last 5 ms of it, stream restarted at the track start. Done in an
// alarm IRQ on core1, so the INDEX pulse isn't missed and the track restart isn't late while the main loop is busy
// (loading tracks from PSRAM after a step, storing a received track, debug output).
#define TRACK_TIME_US       200000
#define INDEX_PULSE_US      5000

static volatile bool indexPulse = false;

int64_t trackRotationAlarm(alarm_id_t id, void* userData)
{
    if(!indexPulse) {               // index pulse start - last 5 ms of the track
        indexPulse = true;
        gpio_put(PIN_INDEX, 0);
        return -INDEX_PULSE_US;     // negative: next alarm relative to when this one should have fired, so no drift
    }

    indexPulse = false;             // index pulse end - next track rotation starts
    gpio_put(PIN_INDEX, 1);
    readTrackData_goToStart();      // move the pointer in the track stream to start
    return -(TRACK_TIME_US - INDEX_PULSE_US);
}

// call on core1 - the alarm IRQ comes to the core that created the alarm pool
void trackRotationSetup(void)
{
    indexPulse = false;
    gpio_put(PIN_INDEX, 1);
    readTrackData_goToStart();

    alarm_pool_t* pool = alarm_pool_create_with_unused_hardware_alarm(1);
    alarm_pool_add_alarm_in_us(pool, TRACK_TIME_US - INDEX_PULSE_US, trackRotationAlarm, NULL, true);
}

void requestTrackLoad(uint16_t track, uint16_t sector);

void updateStreamPositionByFloppyPosition(void)
{
    // How many ms takes to stream 1 sector (whole track is 200 ms)
    // This would ideally be '200 / imgSectorsPerTrack', but this causes sometimes the last sector in track not be streamed
    // and thus *SOMETIMES* produces error reading last sector in track with custom fdc routines in fdd test.
    int timeMsPerSector = 19;

    uint32_t now = millis();
    uint32_t timeSinceTrackStart = now - timeTrackStart;        // how many ms have passed since track start (0 - 200)
    uint32_t timeToTrackEnd = 200 - timeSinceTrackStart;        // how many ms we still have until this track ends (0 - 200)

    // not enough time to stream the last sector completely?
    // set data index to end of stream, will just stream same symbols until the track restarts
    // (this is what could cause last sector in track not be streamed when timeMsPerSector is too big)
    if(timeSinceTrackStart >= 200 || timeToTrackEnd < timeMsPerSector) {
        dataIndexInTrack = READTRACKDATA_SIZE_BYTES - 1;
        return;
    }

    int currentSectorIndex = (timeSinceTrackStart / timeMsPerSector);           // convert timeSinceTrackStart to sector index (0 - 10)
    int currentSectorNo = MIN(currentSectorIndex + 1, imgSectorsPerTrack);      // limit current sector number
    currentSectorNo = MAX(currentSectorNo, 1);                                  // entry 0 of the stream table is the end of stream, not a sector

    // from the stream table of the side being streamed read offset to this sector
    uint8_t* pTrackDataStart = BIT_IS_H(PIN_SIDE1) ? trackData0 : trackData1;   // SIDE1 is H for side 0
    uint32_t currentSectorStartIndex = getWord(pTrackDataStart + (currentSectorNo * 2));

    // offset 0 (sector not in this track) or not pointing into the stream after the table? don't stream the table or
    // anything else as MFM data - stream nothing until the track restarts
    if(currentSectorStartIndex < (2 * STREAM_TABLE_ITEMS) || currentSectorStartIndex >= READTRACKDATA_SIZE_BYTES) {
        dataIndexInTrack = READTRACKDATA_SIZE_BYTES - 1;
        return;
    }

    dataIndexInTrack = currentSectorStartIndex;
}

// Writes are started and ended in the WGATE IRQ (onWriteStart / onWriteEnd), so the capture starts with the first pulse
// even when the core1 main loop is busy (loading PSRAM after a step, storing a received track). The IRQ can't wait for
// a free buffer or use PSRAM, so the main loop (handleWriteBuffers) does that part:
// - it keeps buffers prepared in wrBuffersReady, the IRQ takes one on write start
// - the IRQ puts finished writes into wrBuffersFinished, the main loop stores them locally and passes them to core0
#define WRITE_BUFFERS_READY     2
#define WRITE_BUFFERS_FINISHED  4

static IPCbuffer* volatile wrBuffersReady[WRITE_BUFFERS_READY];
static IPCbuffer* volatile wrBuffersFinished[WRITE_BUFFERS_FINISHED];
static volatile int wrFinishedCount = 0;
volatile uint32_t wrWritesDropped = 0;     // writes not captured - no prepared buffer, or too many finished ones not handled yet

// on WGATE falling edge, from IRQ
void onWriteStart(void)
{
    // WGATE and WDATA are shared by all drives on the ST floppy bus - if our drive isn't selected, this write is for
    // the other drive, so don't capture it (wrBuffer stays NULL, the write IRQ drops the data, onWriteEnd() does nothing)
    if(BIT_IS_H(PIN_DRIVE_SEL)) {
        return;
    }

    if(wrBuffer != NULL) {      // the end of the previous write was missed? finish it first
        onWriteEnd();
    }

    IPCbuffer* bfr = NULL;
    for(int i=0; i<WRITE_BUFFERS_READY; i++) {     // take a buffer prepared by the main loop
        if(wrBuffersReady[i] != NULL) {
            bfr = wrBuffersReady[i];
            wrBuffersReady[i] = NULL;
            break;
        }
    }

    if(bfr == NULL) {           // none prepared (core0 has all buffers - host not connected for long)? can't capture this write
        wrWritesDropped++;
        return;
    }

    posStreamed.side = BIT_IS_H(PIN_SIDE1) ? 0 : 1;         // get the current SIDE

    // Remember the position now, at write start. posStreamed is updated when stream data is put into the DMA ring,
    // which is 5-11 ms ahead of the output, so later it points to the next sector.
    bfr->sideNo   = posStreamed.side;
    bfr->trackNo  = posStreamed.track;
    bfr->sectorNo = posStreamed.sector;

    bfr->data[10] = (((bfr->sideNo > 0) ? 0x80 : 0) | bfr->trackNo);    // store side, track, sector into write positions buffer
    bfr->data[11] = bfr->sectorNo;
    bfr->length = 12;           // start with 12 bytes in the buffer - 10 for header, 2 for track + side + sector

    uint32_t irqState = save_and_disable_interrupts();      // the write DMA IRQ has higher priority, keep it out of this
    mfmWriteCaptureStart();                                 // start byte packing from scratch for this write
    wrBuffer = bfr;                                         // the write IRQ stores the captured data here from now on
    restore_interrupts(irqState);
}

// on WGATE rising edge, from IRQ
void onWriteEnd(void)
{
    uint32_t irqState = save_and_disable_interrupts();      // the write DMA IRQ has higher priority, keep it out of this
    mfmWriteCaptureEnd();                                   // store the rest of the captured pulses into wrBuffer, empty the ring
    IPCbuffer* bfr = (IPCbuffer*) wrBuffer;
    wrBuffer = NULL;                                        // write finished, nothing more goes to this buffer
    restore_interrupts(irqState);

    if(bfr == NULL) {           // this write wasn't captured
        return;
    }

    if(wrFinishedCount < WRITE_BUFFERS_FINISHED) {          // pass it to the main loop
        wrBuffersFinished[wrFinishedCount] = bfr;
        wrFinishedCount++;
    } else {                                                // main loop didn't handle the previous ones yet - can't keep this one
        ipcBufferRelease(bfr);
        wrWritesDropped++;
    }
}

// WGATE IRQ - both edges
void wgateIsr(uint32_t events)
{
    bool fell = (events & GPIO_IRQ_EDGE_FALL) != 0;
    bool rose = (events & GPIO_IRQ_EDGE_RISE) != 0;

    if(fell && rose) {                  // both edges since the IRQ came - the current level tells the order
        if(BIT_IS_L(PIN_WGATE)) {       // writing now: previous write ended, new one started
            onWriteEnd();
            onWriteStart();
        } else {                        // not writing now: a short write started and ended
            onWriteStart();
            onWriteEnd();
        }
    } else if(fell) {
        onWriteStart();
    } else if(rose) {
        onWriteEnd();
    }
}

// Read data output enable: on only while our drive is selected and its motor is on - otherwise another drive on the
// bus may be the one sending data. Set from the current levels, so a missed or merged edge can't leave it wrong.
void updateStreamOutputEnable(void)
{
    bool stWantsTheStream = BIT_IS_L(PIN_DRIVE_SEL) && BIT_IS_L(PIN_MOT_EN);
    gpio_put(PIN_FLCC_OE, stWantsTheStream ? 0 : 1);    // OE is active low
}

// GPIO IRQ callback of core1 - the sdk has one callback per core for all GPIOs
void __isr gpioIsrCore1(uint gpio, uint32_t events)
{
    if(gpio == PIN_STEP) {
        floppyStepISR(gpio, events);
    } else if(gpio == PIN_WGATE) {
        wgateIsr(events);
    } else if(gpio == PIN_DRIVE_SEL || gpio == PIN_MOT_EN) {
        updateStreamOutputEnable();
    }
}

// Output enable follows DRIVE_SEL and MOT_EN in the GPIO IRQ on core1, not in a main loop that can be busy for long.
void streamOutputEnableSetup(void)
{
    gpio_set_irq_enabled_with_callback(PIN_DRIVE_SEL, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, gpioIsrCore1);
    gpio_set_irq_enabled(PIN_MOT_EN, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);

    updateStreamOutputEnable();     // after enabling the IRQ - an edge coming meanwhile then isn't lost
}

// Main loop part of write handling: store finished writes locally, pass them to core0, prepare buffers for next writes.
void handleWriteBuffers(void)
{
    IPCbuffer* finished[WRITE_BUFFERS_FINISHED];

    uint32_t irqState = save_and_disable_interrupts();      // take the finished writes from the IRQ
    int count = wrFinishedCount;
    for(int i=0; i<count; i++) {
        finished[i] = wrBuffersFinished[i];
    }
    wrFinishedCount = 0;
    restore_interrupts(irqState);

    for(int i=0; i<count; i++) {
        IPCbuffer* bfr = finished[i];

        if(bfr->length <= 12) {         // WGATE toggled, but nothing was captured? storing it would clear the sector, host ignores it anyway
            ipcBufferRelease(bfr);
            continue;
        }

        // use the position captured on write start, not the current posStreamed (see onWriteStart)
        storeWrittenSectorDataToTrackLocally(bfr->sideNo, bfr->trackNo, bfr->sectorNo, bfr->data + 12, bfr->length - 12);
        ipcSetBufferAndPutToFifo(bfr, 0, CMD_SEND_WRITTEN_SECTOR, bfr->length);     // store data to buffer, add buffer index to queue
    }

    // prepare buffers for the next writes - without waiting, if core0 has them all, try again on next loop run
    for(int i=0; i<WRITE_BUFFERS_READY; i++) {
        if(wrBuffersReady[i] != NULL) {
            continue;
        }

        IPCbuffer* bfr = ipcGetFreeBuffer(0, 0);
        if(bfr == NULL) {
            break;
        }

        storeHeader(bfr->data, ATN_SECTOR_WRITTEN, 12);     // store header and initial size in header
        bfr->length = 12;
        wrBuffersReady[i] = bfr;        // from now on the IRQ can take it
    }
}

// Start the write capture: PIO + DMA for WDATA, write buffers, WGATE IRQ.
void writeCaptureSetup(void)
{
    uint32_t irqState = save_and_disable_interrupts();
    wrBuffer = NULL;
    for(int i=0; i<WRITE_BUFFERS_READY; i++) {
        wrBuffersReady[i] = NULL;
    }
    wrFinishedCount = 0;
    wrWritesDropped = 0;
    restore_interrupts(irqState);

    pio_mfm_write_setup();
    handleWriteBuffers();               // buffers ready before the first write

    gpio_set_irq_enabled_with_callback(PIN_WGATE, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, gpioIsrCore1);
}

// Store track received from host (bfr->data: track #, side #, then bfr->length - 2 bytes of the encoded track stream)
// to PSRAM, and to the track buffer if it's the track being streamed now.
void storeReceivedTrack(IPCbuffer* bfr)
{
    // Turn the buffer into a whole track: stream from the start, zeros after it. The bytes after the received data
    // are left from the previous track received into this IPC buffer - e.g. a sector the new track doesn't have.
    uint32_t streamLength = (bfr->length > 2) ? MIN(bfr->length - 2, READTRACKDATA_SIZE_BYTES) : 0;
    memmove(bfr->data, bfr->data + 2, streamLength);
    memset(bfr->data + streamLength, 0, READTRACKDATA_SIZE_BYTES - streamLength);

    psramStoreTrack(bfr->trackNo, bfr->sideNo, bfr->data);

    if(bfr->trackNo == hwPosition.track) {          // we've just received the track that is being streamed out?
        uint8_t* pTrack = (bfr->sideNo == 0) ? trackData0 : trackData1;     // pick the correct pointer for this side
        memcpy(pTrack, bfr->data, READTRACKDATA_SIZE_BYTES);               // copy data directly to track buffer
    }
}

static int prevSideNo;

// Core 1 start, from setup1() - after core 0 set up the pins and IPC buffers.
void core1Setup(void)
{
    // SPI initialisation.
    spi_init(spi1, 16000000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // init PSRAM, read ID, test read and write
    psramTest();

    core1running = true;

    clearMfmBuffer();
    trackRotationSetup();

    gpio_set_irq_enabled_with_callback(PIN_STEP, GPIO_IRQ_EDGE_FALL, true, gpioIsrCore1);

    // start mfm output
    setupPwmOutput();
    setupDmaToPwm();

    writeCaptureSetup();

    streamOutputEnableSetup();

    prevSideNo = BIT_IS_H(PIN_SIDE1) ? 0 : 1;
}

// One pass of the core 1 main loop - loop1() calls it again and again.
void core1Loop(void)
{
    uint32_t now = millis();
    uint32_t timeMsSinceLastStep = now - lastStepTime;

    // last STEP happened within last 15 ms?
    if(timeMsSinceLastStep < 15 && state != STATE_STEPPING) {
        clearMfmBuffer();
        state = STATE_STEPPING;         // we're in the stepping state
    }

    // last step happened at least 15 ms ago? we're not stepping anymore
    if(timeMsSinceLastStep >= 15 && state == STATE_STEPPING) {
        // queue_try_add(&fifoToCore0, (const void*) &hwPosition.track);
        state = STATE_LOADING;
    }

    if(state == STATE_LOADING) {
        psramLoadTrack(hwPosition.track, 0, trackData0);
        psramLoadTrack(hwPosition.track, 1, trackData1);

        uint32_t irqState = save_and_disable_interrupts();     // track restart (alarm IRQ) must not come between reading timeTrackStart and setting dataIndexInTrack
        updateStreamPositionByFloppyPosition();
        restore_interrupts(irqState);

        state = STATE_STREAMING;
    }

    hwPosition.side = BIT_IS_H(PIN_SIDE1) ? 0 : 1; // get the current SIDE

    if(prevSideNo != hwPosition.side) {     // on side change, clear the current mfm buffer
        prevSideNo = hwPosition.side;
        clearMfmBuffer();
    }

    // something in the queue for core1? get it, handle it
    IPCbuffer* bfr = ipcGetBufferFromFifo(1);
    if(bfr) {
        // debug("c1: c %d\n", bfr->command);
        switch(bfr->command)
        {
            // store the track track data into PSRAM
            case CMD_STORE_TRACK:
                storeReceivedTrack(bfr);
                break;
        }

        ipcBufferRelease(bfr);
    }

    // store finished writes, prepare buffers for next ones
    handleWriteBuffers();

    beeperLoop();
}
