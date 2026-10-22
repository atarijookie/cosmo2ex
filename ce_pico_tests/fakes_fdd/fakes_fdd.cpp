// State and implementation of the fake platform for ce_pico_fdd, see fakes_fdd.h

#include "fakes_fdd.h"
#include "defs.h"           // PIN_CS of the PSRAM

uint32_t             fakeNowMs = 1000000;   // not 0, the code under test uses 'now - lastTime' differences
uint32_t             fakeMillisStep = 0;
std::function<void()> fakeOnTimeRead;
bool                 fakeIrqsDisabled = false;
uint32_t             fakeIrqsOffMaxMs = 0;
static uint32_t      irqsOffSinceMs;
bool                 fakeGpioLevel[FAKE_GPIO_COUNT];
std::vector<uint8_t> fakePsram(FAKE_PSRAM_SIZE);
std::string          fakeDebugOut;

FakePwmHw fakePwmHw;
FakeDmaHw fakeDmaHw;

int     fakeDmaLastClaimed = -1;
uint8_t fakeIrqPriority[64];

static FakePio fakePio;
static irq_handler_t irqHandlers[64];
static int nextDmaChannel;

struct FakeAlarmPool { int dummy; };
static FakeAlarmPool fakeAlarmPool;

struct FakeAlarm {
    alarm_id_t       id;
    uint64_t         dueUs;
    alarm_callback_t callback;
    void*            userData;
};
static std::vector<FakeAlarm> alarms;
static alarm_id_t nextAlarmId = 1;

static struct {
    dma_channel_config config;
    uint32_t countReload;       // transfer_count, reloaded on self re-trigger
    uint32_t countLeft;
    bool     irqPending;
} dmaChannels[FAKE_DMA_CHANNELS];

//--------------------------------------------------------
// fake PSRAM transaction: CS low, 4 bytes cmd + 24-bit address, then data, CS high
static struct {
    bool     selected;
    uint8_t  hdr[4];
    int      hdrCount;
    uint32_t addr;
} psramTxn;

static void psramTxnByte(uint8_t b)
{
    if (psramTxn.hdrCount < 4) {
        psramTxn.hdr[psramTxn.hdrCount++] = b;

        if (psramTxn.hdrCount == 4) {
            psramTxn.addr = (psramTxn.hdr[1] << 16) | (psramTxn.hdr[2] << 8) | psramTxn.hdr[3];
        }
        return;
    }

    if (psramTxn.hdr[0] == 0x02) {                                  // write command - store data
        fakePsram[psramTxn.addr++ % FAKE_PSRAM_SIZE] = b;
    }
}

static uint8_t psramTxnRead(void)
{
    if (psramTxn.hdr[0] == 0x03) {                                  // read command - return data
        return fakePsram[psramTxn.addr++ % FAKE_PSRAM_SIZE];
    }

    if (psramTxn.hdr[0] == 0x9f) {                                  // read ID - manufacturer ID first
        return 0x0d;
    }

    return 0xff;
}

int spi_write_blocking(spi_inst_t spi, const uint8_t *src, size_t len)
{
    if (spi == spi1 && psramTxn.selected) {
        for (size_t i = 0; i < len; i++) {
            psramTxnByte(src[i]);
        }
    }
    return (int) len;
}

int spi_read_blocking(spi_inst_t spi, uint8_t, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        dst[i] = (spi == spi1 && psramTxn.selected) ? psramTxnRead() : 0xff;
    }
    return (int) len;
}

//--------------------------------------------------------
void gpio_put(uint gpio, bool value)
{
    if (gpio == PIN_CS && !value && fakeGpioLevel[PIN_CS]) {        // CS falling edge starts a new PSRAM transaction
        memset(&psramTxn, 0, sizeof(psramTxn));
    }

    if (gpio == PIN_CS) {
        psramTxn.selected = !value;
    }

    if (gpio < FAKE_GPIO_COUNT) {
        fakeGpioLevel[gpio] = value;
    }
}

bool gpio_get(uint gpio)
{
    return gpio < FAKE_GPIO_COUNT ? fakeGpioLevel[gpio] : false;
}

static gpio_irq_callback_t gpioCallback;
static uint32_t gpioIrqEvents[FAKE_GPIO_COUNT];     // enabled IRQ events per pin

void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled)
{
    if (enabled) {
        gpioIrqEvents[gpio] |= event_mask;
    } else {
        gpioIrqEvents[gpio] &= ~event_mask;
    }
}

void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask, bool enabled, gpio_irq_callback_t callback)
{
    gpio_set_irq_enabled(gpio, event_mask, enabled);
    gpioCallback = callback;
}

void fakeGpioIrq(uint gpio, uint32_t events)
{
    events &= gpioIrqEvents[gpio];

    if (events && gpioCallback && !fakeIrqsDisabled) {
        gpioCallback(gpio, events);
    }
}

void fakeGpioSet(uint gpio, bool level)
{
    bool was = fakeGpioLevel[gpio];
    fakeGpioLevel[gpio] = level;

    if (was != level) {
        fakeGpioIrq(gpio, level ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL);
    }
}

//--------------------------------------------------------
absolute_time_t get_absolute_time(void)
{
    static bool inHook = false;

    fakeNowMs += fakeMillisStep;

    if (fakeOnTimeRead && !inHook) {        // the hook may call code that reads the time again - don't recurse
        inHook = true;
        fakeOnTimeRead();
        inHook = false;
    }

    fakeRunDueAlarms();

    return ((absolute_time_t) fakeNowMs) * 1000;
}

alarm_pool_t *alarm_pool_create_with_unused_hardware_alarm(uint)
{
    return &fakeAlarmPool;
}

alarm_id_t alarm_pool_add_alarm_in_us(alarm_pool_t *, uint64_t us, alarm_callback_t callback, void *user_data, bool)
{
    alarms.push_back({ nextAlarmId, ((uint64_t) fakeNowMs) * 1000 + us, callback, user_data });
    return nextAlarmId++;
}

void fakeRunDueAlarms(void)
{
    static bool inAlarm = false;

    if (inAlarm || fakeIrqsDisabled) {
        return;
    }

    inAlarm = true;

    while (true) {
        uint64_t nowUs = ((uint64_t) fakeNowMs) * 1000;
        auto due = alarms.end();
        for (auto it = alarms.begin(); it != alarms.end(); ++it) {
            if (it->dueUs <= nowUs && (due == alarms.end() || it->dueUs < due->dueUs)) {
                due = it;
            }
        }

        if (due == alarms.end()) {
            break;
        }

        FakeAlarm a = *due;
        alarms.erase(due);

        int64_t again = a.callback(a.id, a.userData);
        if (again != 0) {
            a.dueUs = (again < 0) ? (a.dueUs - again) : (nowUs + again);
            alarms.push_back(a);
        }
    }

    inAlarm = false;
}

uint32_t save_and_disable_interrupts(void)
{
    uint32_t wasDisabled = fakeIrqsDisabled;

    if (!fakeIrqsDisabled) {
        fakeIrqsDisabled = true;
        irqsOffSinceMs = fakeNowMs;
    }

    return wasDisabled;
}

void restore_interrupts(uint32_t status)
{
    if (!status && fakeIrqsDisabled) {      // back to enabled - how long were they off?
        fakeIrqsDisabled = false;
        fakeIrqsOffMaxMs = MAX(fakeIrqsOffMaxMs, fakeNowMs - irqsOffSinceMs);
        fakeRunDueAlarms();                 // IRQs that came while disabled are handled now
    }
}

uint32_t millis(void)
{
    return to_ms_since_boot(get_absolute_time());
}

void sleep_ms(uint32_t ms)
{
    fakeNowMs += ms;
    fakeRunDueAlarms();
}

//--------------------------------------------------------
void uart_putc_raw(uart_inst_t uart, char c)
{
    if (uart == uart0) {
        fakeDebugOut += c;
    }
}

//--------------------------------------------------------
void irq_set_exclusive_handler(uint num, irq_handler_t handler)
{
    irqHandlers[num % 64] = handler;
}

void irq_set_priority(uint num, uint8_t hardware_priority)
{
    fakeIrqPriority[num % 64] = hardware_priority;
}

void fakeFireIrq(uint num)
{
    if (irqHandlers[num % 64]) {
        irqHandlers[num % 64]();
    }
}

int dma_claim_unused_channel(bool)
{
    fakeDmaLastClaimed = nextDmaChannel++ % FAKE_DMA_CHANNELS;
    return fakeDmaLastClaimed;
}

void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_addr, const volatile void *read_addr, uint32_t transfer_count, bool)
{
    dmaChannels[channel].config      = *config;
    dmaChannels[channel].countReload = transfer_count;
    dmaChannels[channel].countLeft   = transfer_count;
    dmaChannels[channel].irqPending  = false;

    fakeDmaHw.ch[channel].write_addr     = (uintptr_t) write_addr;
    fakeDmaHw.ch[channel].read_addr      = (uintptr_t) read_addr;
    fakeDmaHw.ch[channel].transfer_count = transfer_count;
}

bool fakeDmaTransfer(uint channel, uint32_t value)
{
    FakeDmaChannelHw &hw = fakeDmaHw.ch[channel];
    const dma_channel_config &c = dmaChannels[channel].config;
    assert(c.size == DMA_SIZE_32);

    *(uint32_t *) hw.write_addr = value;

    if (c.writeIncrement) {
        uintptr_t next = hw.write_addr + sizeof(uint32_t);

        if (c.ringOnWrite && c.ringBits) {      // ring mode: only the low ringBits of the address change
            uintptr_t mask = (((uintptr_t) 1) << c.ringBits) - 1;
            next = (hw.write_addr & ~mask) | (next & mask);
        }

        hw.write_addr = next;
    }

    if (--dmaChannels[channel].countLeft == 0) {   // block done - self re-trigger with the same count, flag the IRQ
        dmaChannels[channel].countLeft = dmaChannels[channel].countReload;
        dmaChannels[channel].irqPending = true;
        return true;
    }

    return false;
}

bool dma_irqn_get_channel_status(uint, uint channel)
{
    return dmaChannels[channel].irqPending;
}

void dma_irqn_acknowledge_channel(uint, uint channel)
{
    dmaChannels[channel].irqPending = false;
}

bool pio_claim_free_sm_and_add_program_for_gpio_range(const pio_program *, PIO *pio, uint *sm, uint *offset, uint, uint, bool)
{
    *pio = &fakePio;
    *sm = 0;
    *offset = 0;
    return true;
}

//--------------------------------------------------------
void queue_init(queue_t *q, uint element_size, uint element_count)
{
    assert(element_size * element_count <= FAKE_QUEUE_MAX_BYTES);
    memset(q, 0, sizeof(*q));
    q->elementSize = element_size;
    q->capacity = element_count;
}

bool queue_try_add(queue_t *q, const void *data)
{
    if (q->count >= q->capacity) {
        return false;
    }

    uint idx = (q->head + q->count) % q->capacity;
    memcpy(q->data + idx * q->elementSize, data, q->elementSize);
    q->count++;
    return true;
}

bool queue_try_remove(queue_t *q, void *data)
{
    if (q->count == 0) {
        return false;
    }

    memcpy(data, q->data + q->head * q->elementSize, q->elementSize);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

bool queue_is_empty(queue_t *q)
{
    return q->count == 0;
}

//--------------------------------------------------------
void fakeReset(void)
{
    memset(fakeGpioLevel, 0, sizeof(fakeGpioLevel));
    fakeGpioLevel[PIN_CS] = true;                   // PSRAM not selected
    std::fill(fakePsram.begin(), fakePsram.end(), 0);
    memset(&psramTxn, 0, sizeof(psramTxn));
    memset(irqHandlers, 0, sizeof(irqHandlers));
    memset(&fakePwmHw, 0, sizeof(fakePwmHw));
    memset(&fakeDmaHw, 0, sizeof(fakeDmaHw));
    memset(&fakePio, 0, sizeof(fakePio));
    nextDmaChannel = 0;
    fakeDmaLastClaimed = -1;
    memset(dmaChannels, 0, sizeof(dmaChannels));
    memset(fakeIrqPriority, PICO_DEFAULT_IRQ_PRIORITY, sizeof(fakeIrqPriority));
    fakeDebugOut.clear();
    fakeMillisStep = 0;
    fakeOnTimeRead = nullptr;
    fakeIrqsDisabled = false;
    fakeIrqsOffMaxMs = 0;
    gpioCallback = nullptr;
    alarms.clear();
    memset(gpioIrqEvents, 0, sizeof(gpioIrqEvents));
}
