#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/sync.h"

#include "defs.h"
#include "hub75.h"
#include "hub75_frame.h"
#include "hub75.pio.h"

// two frames - one shown by the DMA, the other built; the control channel reads shownFrame at the start of every frame
static uint32_t frames[2][HUB75_FRAME_WORDS];
static uint32_t* volatile shownFrame;

static PIO pio;
static uint sm;
static uint offset;
static int dmaData = -1;
static int dmaCtrl;

static uint8_t brightness = DEFAULT_CONTRAST;
static const uint8_t* lastBuf;

bool hub75Detect(void)
{
    // the OLED board has 2k2 pull-ups on SDA, here GP4 (B1) goes only to a 74HCT245 input - the pull-down wins
    gpio_init(PIN_SDA);
    gpio_set_dir(PIN_SDA, GPIO_IN);
    gpio_pull_down(PIN_SDA);
    busy_wait_us_32(1000);

    bool hub75 = !gpio_get(PIN_SDA);
    gpio_disable_pulls(PIN_SDA);
    return hub75;
}

void hub75Init(void)
{
    // LEDs off right away - the pads' default pull-downs keep OE L (= on) with random data in the panels until now
    gpio_init(PIN_HUB75_OE);
    gpio_put(PIN_HUB75_OE, 1);
    gpio_set_dir(PIN_HUB75_OE, GPIO_OUT);

    uint32_t onCycles = hub75OnCycles(brightness, HUB75_MAX_ON_CYCLES);
    hub75BuildFrame(nullptr, HUB75_COLOR, onCycles, HUB75_MIRROR_X, frames[0]);
    hub75BuildFrame(nullptr, HUB75_COLOR, onCycles, HUB75_MIRROR_X, frames[1]);
    shownFrame = frames[0];

    if(!pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_program, &pio, &sm, &offset, PIN_HUB75_DATA,
                                                         PIN_HUB75_OE - PIN_HUB75_DATA + 1, true)) {
        return;                         // stays dark
    }
    hub75_program_init(pio, sm, offset, PIN_HUB75_DATA, PIN_HUB75_CLK, HUB75_CLKDIV);

    dmaData = dma_claim_unused_channel(true);
    dmaCtrl = dma_claim_unused_channel(true);

    // data: the frame to the PIO, paced by its FIFO, then the control channel restarts it
    dma_channel_config c = dma_channel_get_default_config(dmaData);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&c, dmaCtrl);
    dma_channel_configure(dmaData, &c, &pio->txf[sm], frames[0], HUB75_FRAME_WORDS, false);

    // control: shownFrame -> the data channel's read address, which starts it (the count reloads on each start)
    dma_channel_config cc = dma_channel_get_default_config(dmaCtrl);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    dma_channel_configure(dmaCtrl, &cc, &dma_hw->ch[dmaData].al3_read_addr_trig, &shownFrame, 1, true);
}

// the DMA may still be in the frame shown before the last swap (up to one frame, ~1 ms) - wait until it leaves it
static void waitNotShowing(const uint32_t* frame)
{
    uintptr_t start = (uintptr_t) frame;
    uintptr_t end = start + sizeof(frames[0]);      // after the last word - done, but not restarted yet

    while(true) {
        uintptr_t addr = dma_hw->ch[dmaData].read_addr;
        if(addr < start || addr > end) {
            return;
        }
    }
}

void hub75Show(const uint8_t* buf)
{
    lastBuf = buf;

    if(dmaData < 0) {
        return;
    }

    uint32_t* next = (shownFrame == frames[0]) ? frames[1] : frames[0];
    waitNotShowing(next);

    hub75BuildFrame(buf, HUB75_COLOR, hub75OnCycles(brightness, HUB75_MAX_ON_CYCLES), HUB75_MIRROR_X, next);
    __compiler_memory_barrier();
    shownFrame = next;                  // from the next frame on
}

void hub75SetBrightness(uint8_t value)
{
    if(value == brightness) {
        return;
    }

    brightness = value;
    if(lastBuf) {
        hub75Show(lastBuf);
    }
}
