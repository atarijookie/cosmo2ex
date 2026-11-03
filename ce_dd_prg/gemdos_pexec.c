#include <mint/sysbind.h>
#include <mint/osbind.h>
#include <mint/basepage.h>
#include <mint/ostruct.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ce_dd_prg.h"
#include "xbra.h"
#include "../libacsiscsi/acsi.h"
#include "translated.h"
#include "gemdos.h"
#include "gemdos_errno.h"
#include "bios.h"
#include "main.h"

// * CosmosEx GEMDOS driver by Jookie, 2013 & 2014
// * GEMDOS hooks part (assembler and C) by MiKRO (Miro Kropacek), 2013

// ------------------------------------------------------------------
// init and hooks part - MiKRO
extern int16_t useOldGDHandler;											// 0: use new handlers, 1: use old handlers
extern int16_t useOldBiosHandler;										// 0: use new handlers, 1: use old handlers

extern int32_t (*gemdos_table[256])( void* sp );
extern int32_t (  *bios_table[256])( void* sp );

// ------------------------------------------------------------------
// CosmosEx and Gemdos part - Jookie

#define PE_LOADGO		0
#define PE_LOAD			3
#define PE_GO			4
#define PE_BASEPAGE		5
#define PE_GOTHENFREE	6

typedef struct __attribute__ ((__packed__))
{
	uint32_t lowtpa;
	uint32_t hitpa;
	uint32_t tbase;
	uint32_t tlen;
	uint32_t dbase;
	uint32_t dlen;
	uint32_t bbase;
	uint32_t blen;
	uint32_t dta;
	uint32_t parent;
	uint32_t reserved;
	uint32_t env;
} TBasePage;

typedef struct __attribute__ ((__packed__))
{
	uint16_t  magic;
	uint32_t tsize;
	uint32_t dsize;
	uint32_t bsize;
	uint32_t ssize;
	uint32_t res1;
	uint32_t prgFlags;
	uint16_t  absFlag;
} TPrgHead;

void freeTheBasePage(TBasePage *basePage);
static uint8_t hasGoAndFree(void);
static uint32_t getLongFromBytes(const uint8_t *p);

extern uint16_t pexec_postProc;
extern uint16_t pexec_callOrig;

#define PRG_MAGIC       0x601a
#define PRG_HEAD_SIZE   28


// ------------------------------------------------------------------
// LONG Pexec( mode, fname, cmdline, envstr )
//
// PE_LOADGO and PE_LOAD of a PRG on our drive: the PRG is loaded here (Pexec(PE_BASEPAGE), Fread, relocate, clear BSS),
// the same way as SidecarT GEMDRIVE does it. For PE_LOADGO the asm part then turns the caller's Pexec() into
// Pexec(PE_GOTHENFREE, 0, basepage, 0) and jumps to TOS - the child then owns its memory and TOS frees it when the child
// ends (or keeps it on Ptermres()), so we don't need to hook Pterm() at all. TOS 1.00 / 1.02 don't have PE_GOTHENFREE,
// there the child is run from here with PE_GO and its memory is freed after it returns.
int32_t custom_pexec( void *sp )
{
	uint8_t *params = (uint8_t *) sp;

	// retrieve params from stack
	uint16_t mode    = *((uint16_t *) params);
	params += 2;
	char *fname      = (char *) *((uint32_t *) params);
	params += 4;
	char *cmdline    = (char *) *((uint32_t *) params);
	params += 4;
	char *envstr     = (char *) *((uint32_t *) params);


	// for any other than these modes don't do anything special, just call the original
	if(mode != PE_LOADGO && mode != PE_LOAD) {                              // not one of 2 supported modes? Call original Pexec()
        pexec_callOrig = 1;                                                 // will call the original Pexec() handler from asm when this finishes
        return 0;
	}

	// if we got here, the mode is PE_LOADGO || PE_LOAD
	uint16_t drive = getDriveFromPath((char *) fname);


	if(!isOurDrive(drive, 0)) {												// not our drive? Call original Pexec()
        pexec_callOrig = 1;                                                 // will call the original Pexec() handler from asm when this finishes
        return 0;
	}

	// if we got here, then it's a PRG on our drive...
	uint32_t prgStartBuf[(PRG_HEAD_SIZE + 3) / 4];                          // uint32_t array - aligned to 4 (TT DMA)
	TPrgHead *prgHead = (TPrgHead *) prgStartBuf;

	// open the file and check the header before creating the base page, so a bad file doesn't cost Pexec(PE_BASEPAGE)
	int32_t file = Fopen(fname, 0);											// try to open the file
	if(file < 0) {															// if the handle is negative, fail -- file not found
		return file;
	}

	int32_t diskProgSize = Fseek(0, file, 2);								// seek to end, returns file size
	Fseek(0, file, 0);														// seek to the start

	if(diskProgSize < PRG_HEAD_SIZE || Fread(file, PRG_HEAD_SIZE, prgHead) != PRG_HEAD_SIZE || prgHead->magic != PRG_MAGIC) {
		Fclose(file);
		return extendByteToDword(EPLFMT);														// Invalid program load format
	}

	uint32_t loadSize = diskProgSize - PRG_HEAD_SIZE;                       // text + data + symbols + fixups
	uint32_t imageSize = prgHead->tsize + prgHead->dsize + prgHead->ssize;  // where the fixups start

	if(loadSize < imageSize) {                                              // file shorter than its header says?
		Fclose(file);
		return extendByteToDword(EPLFMT);
	}

	// create base page
	TBasePage *sBasePage = (TBasePage *) Pexec(PE_BASEPAGE, 0, cmdline, envstr);

	if((int32_t) sBasePage < 0 || (uint32_t) sBasePage < 1000) {			// Pexec seems to failed -- insufficient memory
		Fclose(file);
		return extendByteToDword(ENSMEM);
	}

	uint8_t *pBasePage = (uint8_t *) sBasePage;

	// see if it will fit in the free memory - text + data + bss, and the whole file (symbols, fixups) while loading
	uint32_t memProgSize     = prgHead->tsize + prgHead->dsize + prgHead->bsize;
	uint32_t memoryAvailable = sBasePage->hitpa - sBasePage->lowtpa - 0x100;	// without the base page itself

	if(memoryAvailable < memProgSize || memoryAvailable < loadSize) {		// if the program (in RAM or on disk) is bigger than the available free memory
		Fclose(file);
		freeTheBasePage(sBasePage);
		return extendByteToDword(ENSMEM);														// error: insufficient memory
	}

	int32_t got = Fread(file, loadSize, pBasePage + 0x100);					// now read the rest of the file
	Fclose(file);

	if(got != (int32_t) loadSize) {                                         // read failed or got less than expected
		freeTheBasePage(sBasePage);
		return (got < 0) ? got : extendByteToDword(EREADF);
	}

	// fill the base page
	sBasePage->tbase	= sBasePage->lowtpa + 0x100;
	sBasePage->tlen		= prgHead->tsize;
	sBasePage->dbase	= sBasePage->tbase + sBasePage->tlen;
	sBasePage->dlen		= prgHead->dsize;
	sBasePage->bbase	= sBasePage->dbase + sBasePage->dlen;
	sBasePage->blen		= prgHead->bsize;

	// do the addresses fixup if needed (fixups may be missing completely, or be just a zero long)
	if(prgHead->absFlag == 0 && loadSize >= imageSize + 4) {
		uint8_t *fixups      = (uint8_t *) (sBasePage->tbase + imageSize);
		uint8_t *fixupsEnd   = pBasePage + 0x100 + loadSize;
		uint32_t fixupOffset = getLongFromBytes(fixups);					// byte access - no address error if odd
		fixups += 4;

		if(fixupOffset != 0) {						                    // if fixup needed?
			uint8_t *pWhereToFix = (uint8_t *) (sBasePage->tbase + fixupOffset);	// the first uint32_t position that needs to be fixed
			uint8_t *pFixEnd     = (uint8_t *) sBasePage->bbase - 4;   // last valid long in text + data

			while(1) {
				if(pWhereToFix > pFixEnd || ((uint32_t) pWhereToFix & 1)) {	// broken fixups would write outside the program or cause address error
					freeTheBasePage(sBasePage);
					return extendByteToDword(EPLFMT);
				}

				*((uint32_t *) pWhereToFix) += sBasePage->tbase;

				uint8_t fixup;
				do {
					if(fixups >= fixupsEnd) {                          // no terminating zero? take it as end
						fixup = 0;
						break;
					}

					fixup = *fixups++;

					if(fixup == 1) {                                    // just move forward by 0xfe, no fix
						pWhereToFix += 0xfe;
					}
				} while(fixup == 1);

				if(fixup == 0) {                                        // terminate fixup
					break;
				}

				pWhereToFix += fixup;                                   // move forward and fixup
			}
		}
	}

	memset((uint8_t *) sBasePage->bbase, 0, sBasePage->blen);			// clear BSS section

	if(mode == PE_LOAD) {                                               // just load? the caller gets the base page and runs it himself
		return (uint32_t) pBasePage;
	}

	// PE_LOADGO
	if(hasGoAndFree()) {
		pexec_postProc = 1;											    // asm handler does Pexec(PE_GOTHENFREE, 0, basepage, 0) after this function ends
		return (uint32_t) pBasePage;
	}

	// TOS 1.00 / 1.02 - no PE_GOTHENFREE: run it from here, then free its memory
	int32_t res = Pexec(PE_GO, 0, (char *) pBasePage, 0);
	freeTheBasePage(sBasePage);
	return res;
}

// PE_GOTHENFREE (6) is in GEMDOS 0.15 and later (TOS 1.04+), same check as in SidecarT
static uint8_t hasGoAndFree(void)
{
	static int8_t has = -1;

	if(has < 0) {
		uint16_t v   = (uint16_t) Sversion();                           // low byte major, high byte minor
		uint16_t ver = (uint16_t) ((v << 8) | (v >> 8));                // 0xMMmm
		has = (ver >= 0x0015) ? 1 : 0;
	}

	return (uint8_t) has;
}

static uint32_t getLongFromBytes(const uint8_t *p)
{
	return (((uint32_t) p[0]) << 24) | (((uint32_t) p[1]) << 16) | (((uint32_t) p[2]) << 8) | ((uint32_t) p[3]);
}

void freeTheBasePage(TBasePage *basePage)
{
	Mfree((void *) basePage->env);										// free the environment
	Mfree(basePage);												// free the base page
}
