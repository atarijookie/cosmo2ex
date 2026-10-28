#include "defs.h"
#include "bridge.h"
#include "utils.h"

uint8_t hwVariant = VARIANT_SCSI;

// The variant is set by assembly: R125 pulls PIN_VARIANT_STRAP up on ACSI, R225 pulls it down on SCSI.
// On SCSI the pin is also ATN_MSG, but on boot the input chip IC201 is disabled (IN_OE pulled up by R6), so only the strap drives it.
void detectHwVariant(void)
{
    gpio_init(PIN_VARIANT_STRAP);
    gpio_set_dir(PIN_VARIANT_STRAP, GPIO_IN);
    gpio_disable_pulls(PIN_VARIANT_STRAP);      // internal pulls would fight the 100k strap
    sleep_us(100);

    hwVariant = BIT_IS_H(PIN_VARIANT_STRAP) ? VARIANT_ACSI : VARIANT_SCSI;
}

void pioConfigAll(void)
{
    if(IS_ACSI) acsiPioConfigAll();
    else        scsiPioConfigAll();
}

int pioConfig(int newMode, bool force)
{
    return IS_ACSI ? acsiPioConfig(newMode, force) : scsiPioConfig(newMode, force);
}

void resetBridge(void)
{
    if(IS_ACSI) acsiResetBridge();
    else        scsiResetBridge();
}

uint8_t PIO_write(void)
{
    return IS_ACSI ? acsiPIO_write() : scsiPIO_write();
}

void statusAndMsgRead(uint8_t scsiStatusByte)
{
    if(IS_ACSI) acsiStatusRead(scsiStatusByte);
    else        scsiStatusAndMsgRead(scsiStatusByte);
}

void DMA_write_startWithCount(uint32_t transfersCount)
{
    if(IS_ACSI) acsiDMA_write_startWithCount(transfersCount);
    else        scsiDMA_write_startWithCount(transfersCount);
}

uint8_t DMA_write(void)
{
    return IS_ACSI ? acsiDMA_write() : scsiDMA_write();
}

void DMA_writeBlock(uint8_t* bfr, uint32_t cnt)
{
    if(IS_ACSI) acsiDMA_writeBlock(bfr, cnt);
    else        scsiDMA_writeBlock(bfr, cnt);
}

void DMA_read(uint8_t val)
{
    if(IS_ACSI) acsiDMA_read(val);
    else        scsiDMA_read(val);
}

void DMA_readBlock(const uint8_t* bfr, uint32_t cnt)
{
    if(IS_ACSI) acsiDMA_readBlock(bfr, cnt);
    else        scsiDMA_readBlock(bfr, cnt);
}

void DMA_read_waitForEnd(void)
{
    if(IS_ACSI) acsiDMA_read_waitForEnd();
    else        scsiDMA_read_waitForEnd();
}

void neopixelOutput(uint32_t grb)
{
    if(IS_ACSI) acsiNeopixelOutput(grb);
    else        scsiNeopixelOutput(grb);
}
