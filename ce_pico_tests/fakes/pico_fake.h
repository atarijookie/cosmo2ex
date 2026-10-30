// Host replacements for the pico-sdk / arduino-pico pieces used by the ce_pico_hdd_* sources.
// Only declarations and trivial inlines here, the state lives in fakes.cpp and is controlled from the tests.
//
// The *.pio.h files are compiled with PICO_NO_HARDWARE=1 (see makefile), so they only provide the
// instruction arrays - the program objects and helpers they would normally define are provided here.

#ifndef PICO_FAKE_H
#define PICO_FAKE_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

typedef unsigned int uint;

#ifndef MIN
    #define MIN(a, b)   ((b) > (a) ? (a) : (b))
#endif

#ifndef MAX
    #define MAX(a, b)   ((a) > (b) ? (a) : (b))
#endif

//--------------------------------------------------------
// time - fake clock, advanced only by the tests (fakeAdvanceMillis) or automatically by fakeMillisStep on each millis() call
uint32_t millis(void);
void delay(uint32_t ms);
void sleep_ms(uint32_t ms);

typedef int32_t alarm_id_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);

alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t callback, void *user_data, bool fire_if_past);
bool cancel_alarm(alarm_id_t alarm_id);

//--------------------------------------------------------
// multicore, sync, exceptions
uint get_core_num(void);
uint __get_current_exception(void);     // 0 = thread mode, else number of the exception (IRQ) being handled

void multicore_lockout_start_blocking(void);
void multicore_lockout_end_blocking(void);

inline void __dmb(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

typedef struct { int locked; } mutex_t;
inline void mutex_init(mutex_t *m)              { m->locked = 0; }
inline void mutex_enter_blocking(mutex_t *m)    { m->locked++; }
inline void mutex_exit(mutex_t *m)              { m->locked--; }

//--------------------------------------------------------
// pico util/queue - same semantics as the sdk one (fixed element size, fixed capacity, non-blocking try_* calls)
typedef struct {
    uint8_t *data;
    uint     elementSize;
    uint     capacity;
    uint     head;
    uint     count;
} queue_t;

void queue_init(queue_t *q, uint element_size, uint element_count);
void queue_free(queue_t *q);
bool queue_try_add(queue_t *q, const void *data);
bool queue_try_remove(queue_t *q, void *data);
bool queue_is_empty(queue_t *q);
uint queue_get_level(queue_t *q);

//--------------------------------------------------------
// pico_unique_id - the flash unique ID, set by the test in fakeBoardId
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8

typedef struct { uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES]; } pico_unique_board_id_t;

void pico_get_unique_board_id(pico_unique_board_id_t *id_out);

//--------------------------------------------------------
// gpio - no-ops, the tested code uses them only through the LED macros
inline void gpio_put(uint, bool) {}
inline bool gpio_get(uint) { return false; }
inline bool gpio_get_out_level(uint) { return false; }

//--------------------------------------------------------
// raw registers, used by dump_gpio() in ce_pico_hdd/utils.cpp
enum {
    GPIO_FUNC_XIP = 0, GPIO_FUNC_SPI = 1, GPIO_FUNC_UART = 2, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4,
    GPIO_FUNC_SIO = 5, GPIO_FUNC_PIO0 = 6, GPIO_FUNC_PIO1 = 7, GPIO_FUNC_GPCK = 8, GPIO_FUNC_NULL = 0x1f
};

#define IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB     0
#define IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS    0x1f

struct FakeIoBank0 { struct { uint32_t status; uint32_t ctrl; } io[30]; };
struct FakePadsBank0 { uint32_t voltage_select; uint32_t io[30]; };
struct FakeSio { uint32_t gpio_in; uint32_t gpio_out; uint32_t gpio_oe; };

extern FakeIoBank0   fakeIoBank0;
extern FakePadsBank0 fakePadsBank0;
extern FakeSio       fakeSio;

#define io_bank0_hw     (&fakeIoBank0)
#define pads_bank0_hw   (&fakePadsBank0)
#define sio_hw          (&fakeSio)

//--------------------------------------------------------
// PIO - only what the debug uart (uart_tx.pio.h) needs, its output is captured in fakeDebugOut
typedef struct FakePio *PIO;

struct pio_program { const uint16_t *instructions; uint8_t length; int8_t origin; uint8_t pio_version; };

static const struct pio_program uart_tx_program = { nullptr, 0, -1, 0 };
static const struct pio_program uart_rx_program = { nullptr, 0, -1, 0 };

bool pio_claim_free_sm_and_add_program_for_gpio_range(const pio_program *program, PIO *pio, uint *sm, uint *offset, uint gpio_base, uint gpio_count, bool set_gpio_base);

void fakeUartPutc(char c);

inline void uart_tx_program_init(PIO, uint, uint, uint, uint) {}
inline void uart_tx_program_putc(PIO, uint, char c) { fakeUartPutc(c); }
inline void uart_tx_program_puts(PIO, uint, const char *s) { while (*s) fakeUartPutc(*s++); }

#endif
