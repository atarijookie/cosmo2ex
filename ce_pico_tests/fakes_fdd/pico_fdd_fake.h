// Host replacements for the pico-sdk pieces used by the ce_pico_fdd sources.
// Every pico-sdk header in fakes_fdd/ just includes this one. Only declarations and trivial inlines here,
// the state lives in fakes_fdd.cpp and is controlled from the tests through fakes_fdd.h.

#ifndef PICO_FDD_FAKE_H
#define PICO_FDD_FAKE_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cassert>

typedef unsigned int uint;

#define __isr
#define hard_assert(x)      assert(x)
#define PICO_OK             0

//--------------------------------------------------------
// time - fake clock in fakeNowMs, moved only by the tests
typedef uint64_t absolute_time_t;

absolute_time_t get_absolute_time(void);
inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t) (t / 1000); }
void sleep_ms(uint32_t ms);

// alarm pool - alarms fire when the fake clock reaches them (on time reads, sleep_ms, restore_interrupts),
// unless interrupts are disabled, see fakeRunDueAlarms()
typedef int32_t alarm_id_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);
typedef struct FakeAlarmPool alarm_pool_t;
alarm_pool_t *alarm_pool_create_with_unused_hardware_alarm(uint max_timers);
alarm_id_t alarm_pool_add_alarm_in_us(alarm_pool_t *pool, uint64_t us, alarm_callback_t callback, void *user_data, bool fire_if_past);

//--------------------------------------------------------
// gpio - levels in fakeGpioLevel[], gpio_put on the PSRAM CS pin also drives the fake PSRAM
enum { GPIO_FUNC_SPI = 1, GPIO_FUNC_UART = 2, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4, GPIO_FUNC_SIO = 5 };
enum { GPIO_IRQ_EDGE_FALL = 4, GPIO_IRQ_EDGE_RISE = 8 };
#define GPIO_IN     false
#define GPIO_OUT    true

typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t event_mask);

void gpio_put(uint gpio, bool value);
bool gpio_get(uint gpio);
inline void gpio_set_function(uint, int) {}
inline void gpio_set_dir(uint, bool) {}
inline void gpio_pull_up(uint) {}
void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask, bool enabled, gpio_irq_callback_t callback);  // one callback for all pins
void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled);

//--------------------------------------------------------
// irq, sync
typedef void (*irq_handler_t)(void);

#define PICO_HIGHEST_IRQ_PRIORITY   0x00
#define PICO_DEFAULT_IRQ_PRIORITY   0x80
#define PICO_LOWEST_IRQ_PRIORITY    0xff

void irq_set_exclusive_handler(uint num, irq_handler_t handler);
void irq_set_priority(uint num, uint8_t hardware_priority);
inline void irq_set_enabled(uint, bool) {}

uint32_t save_and_disable_interrupts(void);    // tracked in fakeIrqsDisabled / fakeIrqsOffMaxMs
inline void __dmb(void) {}                      // memory barrier - the tests run on one core, nothing to order
void restore_interrupts(uint32_t status);

typedef struct { int dummy; } critical_section_t;

//--------------------------------------------------------
// multicore, flash
inline void flash_safe_execute_core_init(void) {}
inline int  flash_safe_execute(void (*func)(void *), void *param, uint32_t) { func(param); return PICO_OK; }
inline void flash_range_erase(uint32_t, size_t) {}
inline void flash_range_program(uint32_t, const uint8_t *, size_t) {}

#define PICO_FLASH_SIZE_BYTES   (4 * 1024 * 1024)
#define FLASH_SECTOR_SIZE       4096
#define XIP_BASE                ((uintptr_t) 0x10000000)

//--------------------------------------------------------
// pico util/queue - same semantics as the sdk one (fixed element size, fixed capacity, non-blocking try_* calls)
#define FAKE_QUEUE_MAX_BYTES    1024

typedef struct {
    uint8_t data[FAKE_QUEUE_MAX_BYTES];
    uint    elementSize;
    uint    capacity;
    uint    head;
    uint    count;
} queue_t;

void queue_init(queue_t *q, uint element_size, uint element_count);
bool queue_try_add(queue_t *q, const void *data);
bool queue_try_remove(queue_t *q, void *data);
bool queue_is_empty(queue_t *q);

//--------------------------------------------------------
// uart - debug output goes to fakeDebugOut
typedef struct FakeUart *uart_inst_t;
#define uart0   ((uart_inst_t) 0)
#define uart1   ((uart_inst_t) 1)

void uart_putc_raw(uart_inst_t uart, char c);

//--------------------------------------------------------
// spi - spi1 talks to the fake PSRAM (see fakes_fdd.h)
typedef struct FakeSpi *spi_inst_t;
#define spi0    ((spi_inst_t) 0)
#define spi1    ((spi_inst_t) 1)

inline uint spi_init(spi_inst_t, uint baudrate) { return baudrate; }
int spi_write_blocking(spi_inst_t spi, const uint8_t *src, size_t len);
int spi_read_blocking(spi_inst_t spi, uint8_t repeated_tx_data, uint8_t *dst, size_t len);

//--------------------------------------------------------
// pwm - no-ops, the read stream output is not simulated
struct FakePwmSlice { uint32_t csr, div, ctr, cc, top; };
struct FakePwmHw { FakePwmSlice slice[12]; };
extern FakePwmHw fakePwmHw;
#define pwm_hw  (&fakePwmHw)

inline uint pwm_gpio_to_slice_num(uint gpio) { return (gpio >> 1) & 7; }
inline uint pwm_gpio_to_channel(uint gpio) { return gpio & 1; }
inline uint pwm_get_dreq(uint slice) { return slice; }
inline void pwm_set_clkdiv_int_frac(uint, uint8_t, uint8_t) {}
inline void pwm_set_output_polarity(uint, bool, bool) {}
inline void pwm_set_wrap(uint, uint16_t) {}
inline void pwm_set_chan_level(uint, uint, uint16_t) {}
inline void pwm_set_enabled(uint, bool) {}

//--------------------------------------------------------
// dma - transfers are done by the tests through fakeDmaTransfer() (see fakes_fdd.h), which moves write_addr like the
// hardware does: +size per transfer, wrapping in ring mode, self re-trigger with the same count, completion flag per block
enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };
#define DMA_IRQ_0   10
#define DMA_IRQ_1   11
#define FAKE_DMA_CHANNELS   16

typedef struct {
    enum dma_channel_transfer_size size;
    bool writeIncrement;
    bool ringOnWrite;
    uint ringBits;
} dma_channel_config;

struct FakeDmaChannelHw { uintptr_t read_addr; uintptr_t write_addr; uint32_t transfer_count; uint32_t ctrl_trig; };
struct FakeDmaHw { FakeDmaChannelHw ch[FAKE_DMA_CHANNELS]; uint32_t ints0, ints1; };
extern FakeDmaHw fakeDmaHw;
#define dma_hw  (&fakeDmaHw)

int  dma_claim_unused_channel(bool required);
inline dma_channel_config dma_channel_get_default_config(uint) { return dma_channel_config{DMA_SIZE_32, false, false, 0}; }
inline void channel_config_set_transfer_data_size(dma_channel_config *c, enum dma_channel_transfer_size size) { c->size = size; }
inline void channel_config_set_read_increment(dma_channel_config *, bool) {}
inline void channel_config_set_write_increment(dma_channel_config *c, bool incr) { c->writeIncrement = incr; }
inline void channel_config_set_dreq(dma_channel_config *, uint) {}
inline void channel_config_set_ring(dma_channel_config *c, bool write, uint size_bits) { c->ringOnWrite = write; c->ringBits = size_bits; }
inline uint32_t dma_encode_transfer_count_with_self_trigger(uint32_t count) { return count; }
void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_addr, const volatile void *read_addr, uint32_t transfer_count, bool trigger);
inline void dma_channel_set_irq0_enabled(uint, bool) {}
inline void dma_channel_set_irq1_enabled(uint, bool) {}
inline void dma_irqn_set_channel_enabled(uint, uint, bool) {}
inline uint dma_get_irq_num(uint irq_index) { return DMA_IRQ_0 + irq_index; }
bool dma_irqn_get_channel_status(uint irq_index, uint channel);
void dma_irqn_acknowledge_channel(uint irq_index, uint channel);

//--------------------------------------------------------
// pio - only the rx fifo address used as DMA source
struct FakePio { uint32_t rxf[4]; };
typedef FakePio *PIO;

struct pio_program { const uint16_t *instructions; uint8_t length; int8_t origin; uint8_t pio_version; };

bool pio_claim_free_sm_and_add_program_for_gpio_range(const pio_program *program, PIO *pio, uint *sm, uint *offset, uint gpio_base, uint gpio_count, bool set_gpio_base);
inline uint pio_get_dreq(PIO, uint sm, bool is_tx) { return sm * 2 + (is_tx ? 0 : 1); }

#endif
