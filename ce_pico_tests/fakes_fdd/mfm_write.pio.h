// Stand-in for the header pioasm generates from ce_pico_fdd/mfm_write.pio - the PIO program is not simulated,
// the tests put the captured pulse lengths straight into the DMA ring buffer (mfmBufferWrite).

#ifndef MFM_WRITE_PIO_H
#define MFM_WRITE_PIO_H

#include "pico_fdd_fake.h"

static const struct pio_program mfm_write_program = { nullptr, 0, -1, 0 };

static inline void mfm_write_program_init(PIO, uint, uint, uint) {}

#endif
