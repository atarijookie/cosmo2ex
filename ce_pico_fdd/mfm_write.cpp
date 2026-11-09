#include <stdio.h>
#include <stdlib.h>
#include <cstring>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "mfm_write.pio.h"
#include "pico/util/queue.h"
#include "hardware/dma.h"
#include "mfm_write.h"

#include "defs.h"
#include "utils.h"
#include "psram.h"
#include "ipc.h"
#include "mfm_stream.h"

static PIO pioMfmWrite;
static uint smMfmWrite;
static uint offsetMfmWrite;

static uint dma_channel_rx;

#define DMA_IRQ_TO_USE  1

// DMA ring for captured pulse lengths. The IRQ comes every MFM_WRITE_IRQ_EVERY pulses, but the handler processes everything
// from wrProcessedIdx up to the current DMA write position, so a late IRQ (several DMA blocks done) loses nothing as long as
// it comes before the ring wraps.
// Between writes the pulses aren't dropped, the newest MFM_WRITE_KEEP_IDLE stay in the ring: the WGATE IRQ can come late
// (e.g. while the read stream refill IRQ runs), and the pulses of the write before it was noticed are the 00 preamble -
// without it the sector the device patches into its own track copy can't be read back (the FDC can't sync on the data).
// On write start the capture begins at the last 'gap' pulse in the ring - the interval since the previous write, too long
// for a symbol - which is the first pulse of this write. 192 pulses = 0.8 - 1.5 ms of write start latency covered.
#define MFM_WRITE_BUFFER_SIZE   256                             // in uint32_t items, must be power of 2
#define MFM_WRITE_RING_BITS     10                              // log2(MFM_WRITE_BUFFER_SIZE * 4 bytes)
#define MFM_WRITE_IRQ_EVERY     4                               // DMA transfer count - IRQ after this many pulses
#define MFM_WRITE_KEEP_IDLE     192                             // pulses kept in the ring while no write is captured

__attribute__((aligned(MFM_WRITE_BUFFER_SIZE * 4))) uint32_t mfmBufferWrite[MFM_WRITE_BUFFER_SIZE]; // must align for ring mode to work correctly

volatile uint32_t wrProcessedIdx = 0;   // index of the next pulse in mfmBufferWrite to process
volatile uint8_t wrStreamByte = 0;
volatile uint8_t wrBits = 0;

extern volatile IPCbuffer* wrBuffer;

extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];

static inline uint32_t pulseDuration(uint32_t capturedStamp)
{
    return 0xffffffff - capturedStamp;      // calculate the change from start value of X register
}

// take one captured value and turn it into symbol duration
void __not_in_flash_func(updateWriteDataDirect)(uint32_t capturedStamp)
{
    uint32_t capturedDuration = pulseDuration(capturedStamp);

    uint8_t newTime = 0;

    if(capturedDuration < PULSE_TOO_SHORT) {    // if this pulse is too short (less than 2.9 us long)
        return;
    } else if(capturedDuration < PULSE_4US) {   // 4 us? (interval 2.9 us - 4.9 us) (count * 0.1 us + ~1.6 us, see mfm_write.h)
        newTime = MFM_4US;
    } else if(capturedDuration < PULSE_6US) {   // 6 us? (interval 4.9 us - 6.9 us)
        newTime = MFM_6US;
    } else if(capturedDuration < PULSE_8US) {   // 8 us? (interval 6.9 us - 8.9 us)
        newTime = MFM_8US;
    } else {                            // pulse too long? (longer than 8.9 us)
        return;
    }

    wrStreamByte = (wrStreamByte << 2) | newTime;   // append new time to streamByte
    wrBits += 2;            // now got 2 more bits

    if(wrBits >= 8) {       // got 8 bits?
        wrBits = 0;         // don't have bits now

        // got pointer to write buffer and write buffer not full? add byte
        if(wrBuffer != NULL && wrBuffer->length < wrBuffer->maxDataSize) {
            wrBuffer->data[wrBuffer->length] = wrStreamByte;
            wrBuffer->length++;
        }
    }
}

// index in mfmBufferWrite the DMA writes next
static uint32_t __not_in_flash_func(dmaRingIndex)(void)
{
    uintptr_t dmaWriteAddr = (uintptr_t) dma_hw->ch[dma_channel_rx].write_addr;      // next address the DMA will write
    return ((dmaWriteAddr - (uintptr_t) mfmBufferWrite) / sizeof(uint32_t)) % MFM_WRITE_BUFFER_SIZE;
}

// No write captured now: drop only the pulses older than the newest MFM_WRITE_KEEP_IDLE - a write start noticed late
// takes the rest. Call from the DMA IRQ or with interrupts disabled.
static void __not_in_flash_func(trimIdlePulses)(void)
{
    uint32_t dmaIdx = dmaRingIndex();
    uint32_t pending = (dmaIdx - wrProcessedIdx) % MFM_WRITE_BUFFER_SIZE;

    if(pending > MFM_WRITE_KEEP_IDLE) {
        wrProcessedIdx = (dmaIdx - MFM_WRITE_KEEP_IDLE) % MFM_WRITE_BUFFER_SIZE;
    }
}

// Process all pulses the DMA stored in the ring since the last call. Call from the DMA IRQ or with interrupts disabled.
static void __not_in_flash_func(processCapturedPulses)(void)
{
    uint32_t dmaIdx = dmaRingIndex();

    while(wrProcessedIdx != dmaIdx) {
        updateWriteDataDirect(mfmBufferWrite[wrProcessedIdx]);
        wrProcessedIdx = (wrProcessedIdx + 1) % MFM_WRITE_BUFFER_SIZE;
    }
}

// DMA interrupt handler, called when a DMA channel has data
static void __not_in_flash_func(dmaHandlerMfmWrite)(void)
{
    if (dma_irqn_get_channel_status(DMA_IRQ_TO_USE, dma_channel_rx)) {
        dma_irqn_acknowledge_channel(DMA_IRQ_TO_USE, dma_channel_rx);

        if(wrBuffer != NULL) {      // capturing a write
            processCapturedPulses();
        } else {                    // between writes - keep the newest pulses for a write start noticed late
            trimIdlePulses();
        }
    }
}

// On write start, call with interrupts disabled before setting wrBuffer.
// The capture starts at the last 'gap' pulse kept in the ring (too long for a symbol - the interval since the previous
// write, pushed by the first pulse of this one), so the pulses that came before the WGATE IRQ (the 00 preamble) belong
// to the write, and anything older (e.g. a write to the other drive) doesn't. No gap pulse kept (started too long ago)?
// Then everything kept is taken. The byte packing starts from scratch.
void __not_in_flash_func(mfmWriteCaptureStart)(void)
{
    trimIdlePulses();

    uint32_t dmaIdx = dmaRingIndex();
    for(uint32_t i = wrProcessedIdx; i != dmaIdx; i = (i + 1) % MFM_WRITE_BUFFER_SIZE) {
        if(pulseDuration(mfmBufferWrite[i]) >= PULSE_8US) {     // gap - this write starts here (the gap itself is skipped)
            wrProcessedIdx = i;
        }
    }

    wrStreamByte = 0;
    wrBits = 0;
}

// On write end, call with interrupts disabled while wrBuffer is still set.
// Stores the pulses that are in the ring but weren't processed yet (the last DMA block of the write is usually not complete,
// so no IRQ came for it), and the partially filled last byte - padded with 4 us symbols, which fall into the gap after the CRC.
void __not_in_flash_func(mfmWriteCaptureEnd)(void)
{
    processCapturedPulses();

    while(wrBits != 0) {
        updateWriteDataDirect(0xffffffff - (PULSE_TOO_SHORT + PULSE_4US) / 2);    // 4 us pulse
    }

    wrStreamByte = 0;
}

void pio_mfm_write_setup(void)
{
    // This will find a free pio and state machine for our program and load it for us
    // We use pio_claim_free_sm_and_add_program_for_gpio_range (for_gpio_range variant)
    // so we will get a PIO instance suitable for addressing gpios >= 32 if needed and supported by the hardware
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(&mfm_write_program, &pioMfmWrite, &smMfmWrite, &offsetMfmWrite, PIN_WDATA, 1, true);
    hard_assert(success);

    mfm_write_program_init(pioMfmWrite, smMfmWrite, offsetMfmWrite, PIN_WDATA);

    // Setup dma for write
    dma_channel_rx = dma_claim_unused_channel(true);
    dma_channel_config config_rx = dma_channel_get_default_config(dma_channel_rx);

    channel_config_set_transfer_data_size(&config_rx, DMA_SIZE_32);
    channel_config_set_read_increment(&config_rx, false);
    channel_config_set_write_increment(&config_rx, true);
    channel_config_set_dreq(&config_rx, pio_get_dreq(pioMfmWrite, smMfmWrite, false));  // setup dma to read from pio fifo
    channel_config_set_ring(&config_rx, true, MFM_WRITE_RING_BITS);   // enable ring mode on write side, wraps at MFM_WRITE_BUFFER_SIZE dwords

    // enable irq for rx
    dma_irqn_set_channel_enabled(DMA_IRQ_TO_USE, dma_channel_rx, true);

    // 32-bit read from the FIFO
    dma_channel_configure(  dma_channel_rx, &config_rx,
                            mfmBufferWrite,
                            &pioMfmWrite->rxf[smMfmWrite],
                            dma_encode_transfer_count_with_self_trigger(MFM_WRITE_IRQ_EVERY),
                            true); // dma started

    wrProcessedIdx = 0;     // DMA starts writing at mfmBufferWrite[0]

    // enable DMA IRQ handler - highest priority, so it preempts the read stream refill (DMA_IRQ_0), which has ms of slack
    dma_channel_set_irq1_enabled(dma_channel_rx, true);
    irq_set_exclusive_handler(dma_get_irq_num(DMA_IRQ_TO_USE), dmaHandlerMfmWrite);
    irq_set_priority(dma_get_irq_num(DMA_IRQ_TO_USE), PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(dma_get_irq_num(DMA_IRQ_TO_USE), true);
}

// CRC-CCITT of DAM + data, continuing from the 3x A1 before them
static uint16_t updateSectorCrc(uint16_t crc, uint8_t data)
{
    for(int i=0; i<8; i++) {
        crc = ((crc << 1) ^ ((((crc >> 8) ^ (data << i)) & 0x0080) ? 0x1021 : 0));
    }

    return crc;
}

#define CRC_AFTER_3X_A1     0xcdb4

// Decode a captured write (2-bit symbols, as the host gets it) into the raw run of a sector: DAM, data, CRC. Same decoding
// as MfmCachedImage::decodeMfmBuffer() on the host. The CRC is deliberately lenient the same way: with write emulation the
// captured data is often good while the received CRC is off by 1, or the end of it wasn't captured. The stored CRC is
// always the calculated one, so the sector reads back fine then. Data which isn't complete is not good, though.
int mfmDecodeWrittenSector(const uint8_t* symbols, int length, uint8_t* run)
{
    int total = length * 4;
    int index = 0;

    uint32_t sync = 0;                              // find 3x A1
    bool syncFound = false;
    while(index < total) {
        uint8_t time = (symbols[index / 4] >> (6 - 2 * (index % 4))) & 3;
        index++;

        if(time == 0) {                             // invalid time? skip it
            continue;
        }

        sync = ((sync << 2) | time) & 0x0fffffff;   // leave place only for 3x A1 sync bytes

        if(sync == 0xee7b9ee) {
            syncFound = true;
            break;
        }
    }

    if(!syncFound) {
        return MFM_DECODE_FAILED;
    }

    uint16_t crc = CRC_AFTER_3X_A1;
    uint16_t calcedCrc = 0;
    uint16_t recvedCrc = 0;
    int byteOffset = 0;                             // offset of decoded byte after 3x A1
    int bits = 0;
    uint8_t decoded = 0;
    bool remainder = false;                         // got some remainder from the previous time

    while(index < total) {
        uint8_t time = (symbols[index / 4] >> (6 - 2 * (index % 4))) & 3;
        index++;

        uint8_t newBits = 0;
        int newBitsCount;

        if(remainder) {                             // with remainder of '1' from previous time
            switch(time) {
                case MFM_4US: newBits = 0; newBitsCount = 1; remainder = true;  break;
                case MFM_6US: newBits = 1; newBitsCount = 2; remainder = false; break;
                case MFM_8US: newBits = 0; newBitsCount = 2; remainder = true;  break;
                default:      continue;
            }
        } else {                                    // no remainder from previous time
            switch(time) {
                case MFM_4US: newBits = 1; newBitsCount = 1; remainder = false; break;
                case MFM_6US: newBits = 0; newBitsCount = 1; remainder = true;  break;
                case MFM_8US: newBits = 1; newBitsCount = 2; remainder = false; break;
                default:      continue;
            }
        }

        for(int i=newBitsCount-1; i>=0; i--) {
            decoded = (decoded << 1) | ((newBits >> i) & 1);
            bits++;

            if(bits < 8) {
                continue;
            }
            bits = 0;

            if(byteOffset == 0 && decoded == MFM_ID_MARK) {     // ID mark instead of data mark - the track is being formatted
                return MFM_DECODE_FORMAT;
            }

            if(byteOffset == 0 && decoded != MFM_DAM_MARK) {
                return MFM_DECODE_FAILED;
            }

            if(byteOffset <= 512) {
                run[byteOffset] = decoded;
                crc = updateSectorCrc(crc, decoded);
            } else if(byteOffset == 513) {
                calcedCrc = crc;
                recvedCrc = ((uint16_t) decoded) << 8;
            } else if(byteOffset == 514) {
                recvedCrc |= decoded;

                if(recvedCrc != calcedCrc && (((int) recvedCrc) - ((int) calcedCrc)) != 1) {
                    return MFM_DECODE_FAILED;
                }
            }

            byteOffset++;

            if(byteOffset > 514) {                  // the rest is just the gap
                run[513] = crc >> 8;
                run[514] = crc & 0xff;
                return MFM_DECODE_SECTOR;
            }
        }
    }

    if(byteOffset <= 512) {                         // the data didn't make it whole
        return MFM_DECODE_FAILED;
    }

    run[513] = crc >> 8;                            // the CRC didn't make it whole, the data did
    run[514] = crc & 0xff;
    return MFM_DECODE_SECTOR;
}

// FORMAT TRACK: the host clears all the sectors of the image track, do the same here so the track reads the same until
// the next image load. Only when this track is still the one in the buffer - it's not after a step.
static void clearFormattedTrack(uint8_t side, uint8_t track, uint8_t* pTrackStart)
{
    uint8_t run[MFM_SECTOR_RUN_SIZE];
    memset(run, 0, sizeof(run));
    run[0] = MFM_DAM_MARK;

    uint16_t crc = CRC_AFTER_3X_A1;
    for(int i=0; i<513; i++) {
        crc = updateSectorCrc(crc, run[i]);
    }
    run[513] = crc >> 8;
    run[514] = crc & 0xff;

    bool cleared = false;
    uint32_t pos = 0, runSize = 0;
    int markSide = -1, markTrack = -1, markSector = -1;

    while(true) {
        int offset = mfmStreamNextRawRun(pTrackStart, READTRACKDATA_SIZE_BYTES, &pos, &runSize, &markSide, &markTrack, &markSector);

        if(offset < 0) {
            break;
        }

        if(markSide == side && markTrack == track && runSize == MFM_SECTOR_RUN_SIZE) {
            memcpy(pTrackStart + offset, run, MFM_SECTOR_RUN_SIZE);
            cleared = true;
        }
    }

    if(cleared) {
        psramStoreTrack(track, side, pTrackStart);
    }
}

void storeWrittenSectorDataToTrackLocally(uint8_t side, uint8_t track, uint8_t sector, uint8_t* bfr, int len)
{
    // track / side / sector out of bounds?
    if(side > 1 || track >= MAX_TRACKS || sector < 1 || sector > MAX_SECTORS_PER_TRACK) {
        debug("Failed to write side: %d, track: %d, sector: %d - TriSiSe out of bounds!\n", side, track, sector);
        return;
    }

    uint8_t* pTrackStart = (side == 0) ? trackData0 : trackData1;

    uint8_t run[MFM_SECTOR_RUN_SIZE];
    int result = mfmDecodeWrittenSector(bfr, len, run);

    if(result == MFM_DECODE_FORMAT) {
        clearFormattedTrack(side, track, pTrackStart);
        return;
    }

    if(result != MFM_DECODE_SECTOR) {       // the old sector stays, the verify read after the write finds out; host decides on its own
        debug("Failed to write side: %d, track: %d, sector: %d - bad written data!\n", side, track, sector);
        return;
    }

    uint32_t pos = 0, runSize = 0;
    int markSide = -1, markTrack = -1, markSector = -1;

    // find the sector by its mark - it's not there e.g. when the buffer already holds another track after a step
    while(true) {
        int offset = mfmStreamNextRawRun(pTrackStart, READTRACKDATA_SIZE_BYTES, &pos, &runSize, &markSide, &markTrack, &markSector);

        if(offset < 0) {
            debug("Failed to write side: %d, track: %d, sector: %d - sector not in track!\n", side, track, sector);
            return;
        }

        if(markSide == side && markTrack == track && markSector == sector && runSize == MFM_SECTOR_RUN_SIZE) {
            memcpy(pTrackStart + offset, run, MFM_SECTOR_RUN_SIZE);     // same size, so nothing else in the track moves
            psramStoreSector(track, side, offset, run, MFM_SECTOR_RUN_SIZE);
            return;
        }
    }
}
