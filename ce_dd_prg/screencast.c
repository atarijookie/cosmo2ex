// Screencast helpers - what is displayed (resolution, palette, screen address), read straight from video registers,
// so it can be used from VBL
#include <stdint.h>

#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/stdlib.h"
#include "screencast.h"

extern uint8_t machine;		/* MACHINE_* - set by findDevice() */

// ST resolution from the ST shifter mode register

static uint8_t getStRes(void)
{
	uint8_t mode = (*((volatile uint8_t *) 0xffff8260)) & 3;
	return (mode <= 2) ? mode : SCREENCAST_RES_UNKNOWN;		/* 0..2 are SCREENCAST_RES_ST_* */
}

// TT resolution from the TT shifter mode register (bits 10-8)

static uint8_t getTtRes(void)
{
	uint16_t mode = ((*((volatile uint16_t *) 0xffff8262)) >> 8) & 7;

	switch(mode) {
		case 0:	return SCREENCAST_RES_ST_LOW;
		case 1:	return SCREENCAST_RES_ST_MID;
		case 2:	return SCREENCAST_RES_ST_HIGH;
		case 4:	return SCREENCAST_RES_TT_MID;
		case 6:	return SCREENCAST_RES_TT_HIGH;
		case 7:	return SCREENCAST_RES_TT_LOW;
	}
	return SCREENCAST_RES_UNKNOWN;
}

// Falcon resolution from the VIDEL registers: colors from SPSHIFT (or ST shifter mode), width from line width,
// height from vertical display begin / end (in half-lines) and VCO line doubling / interlace bits.
// Width and height are rounded to the nearest of the typical values, so overscan ends up as the closest normal mode.

static uint8_t getFalconRes(void)
{
	uint16_t spshift	= *((volatile uint16_t *) 0xffff8266);
	uint16_t lineWords	= *((volatile uint16_t *) 0xffff8210);		/* VWRAP - line width in words */
	uint16_t vdb		= *((volatile uint16_t *) 0xffff82a8);		/* vertical display begin */
	uint16_t vde		= *((volatile uint16_t *) 0xffff82aa);		/* vertical display end */
	uint16_t vco		= *((volatile uint16_t *) 0xffff82c2);		/* video control */
	uint16_t bpp;
	uint8_t bppBits;

	if(spshift & 0x0400) {						/* 2 colors */
		bpp = 1;	bppBits = SCREENCAST_RES_F_BPP1;
	} else if(spshift & 0x0100) {				/* true color */
		bpp = 16;	bppBits = SCREENCAST_RES_F_BPP16;
	} else if(spshift & 0x0010) {				/* 256 colors */
		bpp = 8;	bppBits = SCREENCAST_RES_F_BPP8;
	} else {									/* 4 or 16 colors are set through ST shifter mode */
		switch((*((volatile uint8_t *) 0xffff8260)) & 3) {
			case 0:	bpp = 4;	bppBits = SCREENCAST_RES_F_BPP4;	break;
			case 1:	bpp = 2;	bppBits = SCREENCAST_RES_F_BPP2;	break;
			case 2:	bpp = 1;	bppBits = SCREENCAST_RES_F_BPP1;	break;
			default: return SCREENCAST_RES_UNKNOWN;
		}
	}

	uint16_t width = (lineWords * 16) / bpp;

	uint16_t lines = (vde > vdb) ? ((vde - vdb) / 2) : 0;	/* half-lines to lines */
	if(vco & 1) {								/* line doubling - each line shown twice */
		lines /= 2;
	}
	if(vco & 2) {								/* interlace - twice the lines */
		lines *= 2;
	}

	if(width == 0 || lines == 0) {
		return SCREENCAST_RES_UNKNOWN;
	}

	uint8_t isWide = (width >= 480);
	uint16_t height;
	uint8_t heightBits;

	if(lines < 220) {
		height = 200;	heightBits = SCREENCAST_RES_F_H200;
	} else if(lines < 320) {
		height = 240;	heightBits = SCREENCAST_RES_F_H240;
	} else if(lines < 440) {
		height = 400;	heightBits = SCREENCAST_RES_F_H400;
	} else {
		height = 480;	heightBits = SCREENCAST_RES_F_H480;
	}

	/* ST compatible layouts are reported as ST resolutions */
	if(!isWide && height == 200 && bpp == 4) {
		return SCREENCAST_RES_ST_LOW;
	}
	if(isWide && height == 200 && bpp == 2) {
		return SCREENCAST_RES_ST_MID;
	}
	if(isWide && height == 400 && bpp == 1) {
		return SCREENCAST_RES_ST_HIGH;
	}

	return SCREENCAST_RES_FALCON_MODE(isWide ? SCREENCAST_RES_F_W640 : SCREENCAST_RES_F_W320, heightBits, bppBits);
}

uint8_t screencastGetRes(void)
{
	switch(machine) {
		case MACHINE_TT:		return getTtRes();
		case MACHINE_FALCON:	return getFalconRes();
	}
	return getStRes();
}

// size of screen data which is sent for this resolution - the not supported yet ones send ST screen

uint32_t screencastScreenSize(uint8_t res)
{
	switch(res) {
		case SCREENCAST_RES_TT_LOW:
		case SCREENCAST_RES_TT_MID:
		case SCREENCAST_RES_TT_HIGH:
			return SCREENCAST_SCREEN_SIZE_TT;
	}
	return SCREENCAST_SCREEN_SIZE_ST;
}

// copy palette of this resolution to bfr, returns its size in bytes

uint32_t screencastGetPalette(uint8_t res, uint8_t *bfr)
{
	volatile uint16_t *ttPalette = (volatile uint16_t *) 0xffff8400;	/* 256 TT colors */
	uint16_t bank;

	switch(res) {
		case SCREENCAST_RES_TT_LOW:					/* all 256 TT colors */
			memcpy(bfr, (void *) ttPalette, 256*2);
			return 256*2;

		case SCREENCAST_RES_TT_MID:					/* 16 colors of the bank selected in TT shifter mode register */
			bank = (*((volatile uint16_t *) 0xffff8262)) & 0x0f;
			memcpy(bfr, (void *) (ttPalette + bank * 16), 16*2);
			return 16*2;
	}

	/* ST resolutions (on TT the ST palette registers show the selected bank), TT high is just black and white */
	memcpy(bfr, (void *) 0xffff8240, 16*2);
	return 16*2;
}

// the displayed screen from video base registers (not _v_bas_ad), so it's right also when a program sets them directly;
// TT and Falcon have also the low byte, on ST it's always 256 bytes aligned

uint8_t *screencastScreenAddress(void)
{
	uint32_t addr = ((uint32_t) *((volatile uint8_t *) 0xffff8201) << 16) | ((uint32_t) *((volatile uint8_t *) 0xffff8203) << 8);

	if(machine == MACHINE_TT || machine == MACHINE_FALCON) {
		addr |= *((volatile uint8_t *) 0xffff820d);
	}
	return (uint8_t *) addr;
}
