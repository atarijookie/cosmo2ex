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
#include "hardware/clocks.h"

#include "defs.h"
#include "utils.h"
#include "display.h"
#include "psram.h"
#include "ipc.h"
#include "mfm_write.h"
#include "core1_main.h"
#include "beeper.h"
#include "mfm_stream.h"
#include "mfm_perf.h"

volatile bool core1running = false;

#define MFM_BUFFER_SIZE         2048                                // 12 address bits masked for ring mode (4096 bytes == 2048 words)
#define MFM_BUFFER_HALF_SIZE    (MFM_BUFFER_SIZE / 2)

__attribute__((aligned(4096))) uint16_t mfmBuffer[MFM_BUFFER_SIZE]; // must align to 4096 for ring mode to work correctly

int dmaChannel;
volatile bool halfDone = false;

#define FILL_NONE   0
#define FILL_LOWER  1
#define FILL_UPPER  2

volatile uint8_t fillWhat = FILL_NONE;

extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];

// The track being streamed. Only the refill (DMA IRQ) touches it while streamValid is true. The stream isn't valid after a
// step, a side change or a new track in the buffer: the same offset means something else then, so the refill sends just
// the filler until the main loop finds the position again (seekStreamToCurrentAngle).
MfmStream mfmStream;
volatile bool streamValid = false;
volatile bool trackRestart = false;     // new rotation started, the next refill starts at the track start
volatile int streamSide = 0;
volatile uint32_t lastStepTime = 0;
volatile uint32_t stepCount = 0;          // STEP pulses taken (motor on) - for the status in the debug log
volatile uint32_t mfmHalfCount = 0;       // MFM buffer halves streamed (DMA IRQs) - for the status in the debug log
volatile uint32_t mfmUnderruns = 0;       // refill finished after the DMA got into the refilled half - the output had old data
volatile uint32_t mfmLateMax = 0;         // the latest DMA IRQ so far: how many intervals into the half the DMA was
volatile uint32_t mfmGapMaxUs = 0;        // the longest time between DMA IRQs so far
volatile uint32_t timeTrackStartUs;

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
void __isr __not_in_flash_func(floppyStepISR)(uint gpio, uint32_t event_mask)
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
    stepCount++;

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

  uint32_t clockHz = clock_get_hz(clk_sys);
  pwm_set_clkdiv_int_frac(pwmSliceNum, 1, 0);       // counts at the CPU clock - the interval lengths in the stream are that fine
  pwm_set_output_polarity(pwmSliceNum, true, true);

  pwm_set_wrap(pwmSliceNum, clockHz / 250000 - 1);  // Set period (timer going from from 0 to this value) - 4 us
  pwm_set_chan_level(pwmSliceNum, pwmChannel, clockHz / 2000000);   // Set channel output high for 0.5 us before dropping

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
void __isr __not_in_flash_func(dmaHandlerMfm)(void)
{
    // Clear the interrupt
    dma_hw->ints0 = 1u << dmaChannel;

    static uint32_t lastIrqUs = 0;
    uint32_t irqStartUs = time_us_32();
    uint32_t gapUs = irqStartUs - lastIrqUs;
    lastIrqUs = irqStartUs;

    if(mfmHalfCount > 0) {          // the first one has no previous one to compare to
        mfmGapMaxUs = MAX(mfmGapMaxUs, gapUs);
    }

    mfmHalfCount++;

    // Fill the half the DMA isn't reading now - taken from its read address, not by flipping a flag on each IRQ: when
    // the IRQs are blocked for longer than one half (core 1 idled for a flash write, e.g. EEPROM.commit() on a settings
    // change), several of them merge into one, and a flag would then stay on the wrong half - filling the half being
    // played out, for good.
    uintptr_t readOffset = (uintptr_t) dma_hw->ch[dmaChannel].read_addr - (uintptr_t) mfmBuffer;
    bool readingUpper = (readOffset % sizeof(mfmBuffer)) >= (MFM_BUFFER_HALF_SIZE * sizeof(mfmBuffer[0]));
    halfDone = readingUpper;    // the lower half was played out last

    uint32_t intoHalf = (readOffset % (MFM_BUFFER_HALF_SIZE * sizeof(mfmBuffer[0]))) / sizeof(mfmBuffer[0]);
    mfmLateMax = MAX(mfmLateMax, intoHalf);

    fillWhat = readingUpper ? FILL_LOWER : FILL_UPPER;

    if(state == STATE_STREAMING) {
        uint32_t fillStartUs = time_us_32();
        fillHalfMfmBuffer(fillWhat);
        mfmPerfRecordRefill(time_us_32() - fillStartUs);
    }

    mfmPerfOnRefill();

    // the DMA got into the half we were filling meanwhile? then the output had old data
    uintptr_t readOffsetNow = (uintptr_t) dma_hw->ch[dmaChannel].read_addr - (uintptr_t) mfmBuffer;
    bool readingUpperNow = (readOffsetNow % sizeof(mfmBuffer)) >= (MFM_BUFFER_HALF_SIZE * sizeof(mfmBuffer[0]));

    if(readingUpperNow != readingUpper) {
        mfmUnderruns++;
    }
}

static inline uint8_t* trackDataOfSide(int side)
{
    return (side == 0) ? trackData0 : trackData1;
}

void __not_in_flash_func(fillHalfMfmBuffer)(uint8_t what)
{
    uint16_t* bfr = (what == FILL_LOWER) ? &mfmBuffer[0] : &mfmBuffer[MFM_BUFFER_HALF_SIZE];

    // update SIDE var
    hwPosition.side = BIT_IS_H(PIN_SIDE1) ? 0 : 1; // get the current SIDE

    if(trackRestart) {                      // new rotation - from the track start of the current side
        trackRestart = false;
        streamSide = hwPosition.side;
        mfmStreamStart(&mfmStream, trackDataOfSide(streamSide), READTRACKDATA_SIZE_BYTES);
        streamValid = true;
    } else if(hwPosition.side != streamSide) {  // the other side has a different layout - main loop finds the position
        streamValid = false;
    }

    if(!streamValid) {
        mfmStreamFiller(bfr, MFM_BUFFER_HALF_SIZE);
        return;
    }

    mfmStreamFill(&mfmStream, bfr, MFM_BUFFER_HALF_SIZE);

    posStreamed.side    = mfmStream.side;
    posStreamed.track   = mfmStream.trackNo;
    posStreamed.sector  = mfmStream.sector;
}

void clearMfmBuffer(void)
{
    // copy 'all 4 us' pulses into current streaming buffer to allow shortest possible switch to start of track
    mfmStreamFiller(mfmBuffer, MFM_BUFFER_SIZE);
}

void __not_in_flash_func(readTrackData_goToStart)(void)
{
    trackRestart = true;                        // the next refill starts at the track start
    timeTrackStartUs = time_us_32();            // time of track start to now
}

// Track rotation: 200 ms per track, INDEX low for the last 5 ms of it, stream restarted at the track start. Done in an
// alarm IRQ on core1, so the INDEX pulse isn't missed and the track restart isn't late while the main loop is busy
// (loading tracks from PSRAM after a step, storing a received track, debug output).
#define TRACK_TIME_US       200000
#define INDEX_PULSE_US      5000

static volatile bool indexPulse = false;

int64_t __not_in_flash_func(trackRotationAlarm)(alarm_id_t id, void* userData)
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

// How long the seek takes at most, the stream continues from there - measured on the worst case track, see mfm_perf.h
#define SEEK_TIME_US    3000

// Find where the disk is now in the track of the current side, by going through the stream up to that time. This works
// for any track layout, but takes ms - so with the IRQs enabled, on a copy of the stream, while the refill sends the filler.
void seekStreamToCurrentAngle(void)
{
    uint32_t irqState = save_and_disable_interrupts();
    streamValid = false;
    trackRestart = false;
    uint32_t startUs = timeTrackStartUs;
    MfmStream stream = mfmStream;       // keeps the random state going
    restore_interrupts(irqState);

    int side = BIT_IS_H(PIN_SIDE1) ? 0 : 1;

    mfmStreamStart(&stream, trackDataOfSide(side), READTRACKDATA_SIZE_BYTES);
    mfmStreamSeek(&stream, (time_us_32() - startUs) + SEEK_TIME_US);

    irqState = save_and_disable_interrupts();

    if(!trackRestart && side == (BIT_IS_H(PIN_SIDE1) ? 0 : 1)) {    // new rotation or other side meanwhile? this position is useless
        mfmStream = stream;
        streamSide = side;
        streamValid = true;
    }

    restore_interrupts(irqState);
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
volatile uint32_t wrWritesCaptured = 0;    // writes captured and passed to core0 - for the status in the debug log
volatile uint32_t wrWritesEmpty = 0;       // WGATE toggled, but no data captured - for the status in the debug log

// on WGATE falling edge, from IRQ
void __not_in_flash_func(onWriteStart)(void)
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
void __not_in_flash_func(onWriteEnd)(void)
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
void __not_in_flash_func(wgateIsr)(uint32_t events)
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
void __not_in_flash_func(updateStreamOutputEnable)(void)
{
    bool stWantsTheStream = BIT_IS_L(PIN_DRIVE_SEL) && BIT_IS_L(PIN_MOT_EN);
    gpio_put(PIN_FLCC_OE, stWantsTheStream ? 0 : 1);    // OE is active low
}

// GPIO IRQ callback of core1 - the sdk has one callback per core for all GPIOs
void __isr __not_in_flash_func(gpioIsrCore1)(uint gpio, uint32_t events)
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
            wrWritesEmpty++;
            debug("write t %d s %d sec %d: WGATE toggled, but nothing captured - dropped\n", bfr->trackNo, bfr->sideNo, bfr->sectorNo);
            ipcBufferRelease(bfr);
            continue;
        }

        // use the position captured on write start, not the current posStreamed (see onWriteStart)
        storeWrittenSectorDataToTrackLocally(bfr->sideNo, bfr->trackNo, bfr->sectorNo, bfr->data + 12, bfr->length - 12);
        ipcSetBufferAndPutToFifo(bfr, 0, CMD_SEND_WRITTEN_SECTOR, bfr->length);     // store data to buffer, add buffer index to queue
        wrWritesCaptured++;
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
        if(bfr->sideNo == streamSide) {             // its layout can be different - main loop finds the position again
            streamValid = false;
        }

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

    mfmStreamGlobalInit(clock_get_hz(clk_sys));
    mfmPerfInit();

    clearMfmBuffer();
    trackRotationSetup();
    beeperCore1Init();                  // before the STEP IRQ - the step clicks need its alarm

    gpio_set_irq_enabled_with_callback(PIN_STEP, GPIO_IRQ_EDGE_FALL, true, gpioIsrCore1);

    // start mfm output
    setupPwmOutput();
    setupDmaToPwm();

    writeCaptureSetup();

    streamOutputEnableSetup();

    prevSideNo = BIT_IS_H(PIN_SIDE1) ? 0 : 1;
}

// Floppy status for the debug log - from core 0, about once a second while the ST uses the drive (selected or motor
// on), and once more after it stopped: the drive lines from the ST, the output enable, where the head is, which sector
// is being streamed, the stream state (0 streaming, 1 stepping, 2 loading), how many steps / MFM buffer halves so far,
// writes captured / dropped / empty (WGATE toggled without data) so far.
void core1StatusReport(int imageState)
{
    static bool wasActive = false;

    bool sel = BIT_IS_L(PIN_DRIVE_SEL);
    bool mot = BIT_IS_L(PIN_MOT_EN);
    bool active = sel || mot;

    if(!active && !wasActive) {
        return;
    }
    wasActive = active;

    debug("fdd: sel %d mot %d oe %d wg %d | head t %d s %d | stream t %d s %d sec %d | state %d | steps %u halves %u | wr %u drop %u empty %u | img %d %dx%dx%d\n",
          sel, mot, BIT_IS_L(PIN_FLCC_OE), BIT_IS_L(PIN_WGATE),
          hwPosition.track, hwPosition.side, posStreamed.track, posStreamed.side, posStreamed.sector, (int) state,
          (unsigned) stepCount, (unsigned) mfmHalfCount, (unsigned) wrWritesCaptured, (unsigned) wrWritesDropped, (unsigned) wrWritesEmpty,
          imageState, imgTracks, imgSides, imgSectorsPerTrack);
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

        seekStreamToCurrentAngle();

        state = STATE_STREAMING;
    }

    hwPosition.side = BIT_IS_H(PIN_SIDE1) ? 0 : 1; // get the current SIDE

    if(prevSideNo != hwPosition.side) {     // on side change, clear the current mfm buffer
        prevSideNo = hwPosition.side;
        streamValid = false;
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

    // position in the stream lost (side change, new track)? find it - unless a new rotation starts at the track start anyway
    if(state == STATE_STREAMING && !streamValid && !trackRestart) {
        seekStreamToCurrentAngle();
    }

    // store finished writes, prepare buffers for next ones
    handleWriteBuffers();

    mfmPerfLoop();

    beeperLoop();
}
