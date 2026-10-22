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

static PIO pioMfmWrite;
static uint smMfmWrite;
static uint offsetMfmWrite;

static uint dma_channel_rx;

#define DMA_IRQ_TO_USE  1

// DMA ring for captured pulse lengths. The IRQ comes every MFM_WRITE_IRQ_EVERY pulses, but the handler processes everything
// from wrProcessedIdx up to the current DMA write position, so a late IRQ (several DMA blocks done) loses nothing as long as
// it comes before the ring wraps - 64 pulses = 250-500 us of slack.
#define MFM_WRITE_BUFFER_SIZE   64                              // in uint32_t items, must be power of 2
#define MFM_WRITE_RING_BITS     8                               // log2(MFM_WRITE_BUFFER_SIZE * 4 bytes)
#define MFM_WRITE_IRQ_EVERY     4                               // DMA transfer count - IRQ after this many pulses

__attribute__((aligned(MFM_WRITE_BUFFER_SIZE * 4))) uint32_t mfmBufferWrite[MFM_WRITE_BUFFER_SIZE]; // must align for ring mode to work correctly

volatile uint32_t wrProcessedIdx = 0;   // index of the next pulse in mfmBufferWrite to process
volatile uint8_t wrStreamByte = 0;
volatile uint8_t wrBits = 0;

extern volatile IPCbuffer* wrBuffer;

extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];

// take one captured value and turn it into symbol duration
void updateWriteDataDirect(uint32_t capturedStamp)
{
    uint32_t capturedDuration = 0xffffffff - capturedStamp;  // calculate the change from start value of X register

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

// Process all pulses the DMA stored in the ring since the last call. Call from the DMA IRQ or with interrupts disabled.
static void processCapturedPulses(void)
{
    uintptr_t dmaWriteAddr = (uintptr_t) dma_hw->ch[dma_channel_rx].write_addr;      // next address the DMA will write
    uint32_t dmaIdx = ((dmaWriteAddr - (uintptr_t) mfmBufferWrite) / sizeof(uint32_t)) % MFM_WRITE_BUFFER_SIZE;

    while(wrProcessedIdx != dmaIdx) {
        updateWriteDataDirect(mfmBufferWrite[wrProcessedIdx]);
        wrProcessedIdx = (wrProcessedIdx + 1) % MFM_WRITE_BUFFER_SIZE;
    }
}

// DMA interrupt handler, called when a DMA channel has data
static void dmaHandlerMfmWrite(void)
{
    if (dma_irqn_get_channel_status(DMA_IRQ_TO_USE, dma_channel_rx)) {
        dma_irqn_acknowledge_channel(DMA_IRQ_TO_USE, dma_channel_rx);
        processCapturedPulses();
    }
}

// On write start, call with interrupts disabled before setting wrBuffer.
// Symbols collected so far (noise, or the start of this write the IRQ already dropped) must not shift the byte packing
// of this write. Pulses still waiting in the ring belong to this write - the ring is emptied on every write end.
void mfmWriteCaptureStart(void)
{
    wrStreamByte = 0;
    wrBits = 0;
}

// On write end, call with interrupts disabled while wrBuffer is still set.
// Stores the pulses that are in the ring but weren't processed yet (the last DMA block of the write is usually not complete,
// so no IRQ came for it), and the partially filled last byte - padded with 4 us symbols, which fall into the gap after the CRC.
void mfmWriteCaptureEnd(void)
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

void storeWrittenSectorDataToTrackLocally(uint8_t side, uint8_t track, uint8_t sector, uint8_t* bfr, int len)
{
    // track / side / sector out of bounds?
    if(side > 1 || track >= MAX_TRACKS || sector < 1 || sector > MAX_SECTORS_PER_TRACK) {
        debug("Failed to write side: %d, track: %d, sector: %d - TriSiSe out of bounds!\n", side, track, sector);
        return;
    }

    uint8_t* pTrackStart = (side == 0) ? trackData0 : trackData1;
    uint8_t* pData = pTrackStart;

    uint8_t* pStreamTableItem = pData + (sector * 2);       // calc pointer to where the offset to sector is - in the stream table
    uint32_t offsetToSector = getWord(pStreamTableItem);    // get offset to sector in stream from stream table

    // make sure that the whole sector is after the stream table and within the track data (offset 0 - sector not in track)
    if(offsetToSector < (2 * STREAM_TABLE_ITEMS) || offsetToSector > (READTRACKDATA_SIZE_BYTES - ENCODED_SECTOR_MAX_SIZE)) {
        debug("Failed to write side: %d, track: %d, sector: %d - bad sector offset!\n", side, track, sector);
        return;
    }

    // advance pointer to start of the sector - this address should contain CMD_CURRENT_SECTOR
    pData += offsetToSector;
    uint8_t* pSectorEnd = pData + ENCODED_SECTOR_MAX_SIZE - 1;  // end of sector should be exactly here, because using fixed encoded sector size

    uint8_t* pSectorDataStart = NULL;

    #define HEADER_START    0
    #define DATA_START      1
    #define NOTHING         3

    uint8_t lookingFor = HEADER_START;      // look for header, then for data start, then for data end

    // find start and end of the sector we want to overwrite - only within this sector, so the data start can't be beyond its end
    while(pData + 3 < pSectorEnd) {         // + 3: the current_sector mark is 4 bytes long
        uint8_t val = *pData;

        // end of the track stream - the sector isn't here (e.g. the track buffer already holds another track after a step)
        if(val == CMD_TRACK_STREAM_END) {
            break;
        }

        // current_sector mark
        if(val == CMD_CURRENT_SECTOR) {
            if(lookingFor == DATA_START) {  // header found, but next sector starts before its data start? sector has no data part
                break;
            }

            if(pData[1] == side && pData[2] == track && pData[3] == sector) {   // side + track + sector match? found header!
                lookingFor = DATA_START;    // header found, look for data start
            }
            pData += 4; // skip the current_sector mark - to continue after it
            continue;
        }

        // if we already found the header, but not the sector data start, and this is start of the data, we've found start now
        if(lookingFor == DATA_START && val == CMD_DATA_PART_OF_SECTOR) {
            pData++;                    // skip beyond marker
            pSectorDataStart = pData;   // store pointer to sector data start
            lookingFor = NOTHING;       // data start found, look for data end
            break;
        }

        // mfm data, or data part mark of a sector we're not looking for - move to next data
        pData++;
    }

    // if we didn't find everything needed, fail
    if(lookingFor != NOTHING) {
        debug("Failed to write side: %d, track: %d, sector: %d - no data start found!\n", side, track, sector);
        return;
    }

    int dataSizeBytes = pSectorEnd - pSectorDataStart;  // how many bytes are allocated in buffer for the sector data (from end to start) - should be about 1100 bytes
    int copySize = MIN(len, dataSizeBytes);         // pick smaller of these sizes, so we won't overwrite the start of next sector

    int dataSizeToClear = dataSizeBytes - copySize; // how many bytes should be cleared
    dataSizeToClear = MAX(dataSizeToClear, 0);      // make sure it's not negative

    memcpy(pSectorDataStart, bfr, copySize);        // copy new data at the start of data part of the sector

    if(dataSizeToClear > 0) {
        memset(pSectorDataStart + copySize, 0, dataSizeToClear);    // clear the bytes up to end of sector data
    }

    // copy the new data to psram
    int byteOffsetFromTrackStart = pSectorDataStart - pTrackStart;  // calculate the byte offset of the written sector data from start of the track
    psramStoreSector(track, side, byteOffsetFromTrackStart, bfr, copySize, dataSizeToClear);
}
