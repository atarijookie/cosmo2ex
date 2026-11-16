| Boot status sector of ce_pico_hdd: shows the device status on the Atari while the device isn't connected to the host yet.
|
| Built by make_bootsector.py into ../boot_status_data.h. It's the root sector (sector 0) of the lowest enabled ACSI / SCSI ID
| while the device is not connected - see ../boot_status.cpp. Code + data must fit below the partition table (0x1c2), the
| device patches devno (offset 2) and the checksum word (0x1fe) before sending it.
|
| Once a second it reads sectors 0 + 1 of its own device - with XBIOS DMAread() on TOS 2.00+, with direct ACSI DMA on
| TOS 1.x. While not connected the device answers sector 1 with:
|   0..7    'CE2XSTAT'
|   8       status code
|   9       change counter - the text is printed again when this or the code changes
|   16..    text to print (VT52), 0 terminated
| When sector 1 is something else, the host answers now: sector 0 in the buffer is then the real root sector (e.g. the
| CE_DD boot sector of a translated drive). When it's executable (checksum 0x1234), it's run from the buffer with the
| registers TOS gave us - the boot goes on as if the device had been ready from the start. Not executable, or 3 failed
| reads in a row: just return to TOS. SPACE stops the waiting too.
|
| TOS 2.00+ always has DMAread (it returns 0 or an error), so the direct ACSI code is used just on TOS 1.x - only ST / STE,
| ACSI only.
|
| Runs in supervisor mode, GEMDOS is up already (Malloc, Cconws work).

    .text

start:
    bra.s   boot
devno:
    .dc.b   0                   | offset 2 - set by the device: ACSI 0..7, SCSI 8..15 (as DMAread wants it)
    .dc.b   0                   | reserved

boot:
    movem.l d0-d7/a0-a6,-(sp)   | registers from TOS - restored before running the real root sector

    pea     1024+16.w           | 2 sectors, aligned to 16 B (TT SCSI DMA needs at least longword alignment)
    move.w  #0x48,-(sp)         | Malloc()
    trap    #1
    addq.l  #6,sp
    tst.l   d0
    beq     bootEnd             | no memory? just boot on
    movea.l d0,a4               | a4: block for Mfree()
    moveq   #15,d1
    add.l   d1,d0
    and.w   #0xfff0,d0
    movea.l d0,a5               | a5: aligned buffer

    moveq   #-1,d7              | d7: code + counter shown now, -1 nothing
    moveq   #3,d6               | d6: failed reads left

readStatus:
    bsr     readSectors
    bne.s   readFailed

    moveq   #3,d6
    cmpi.l  #0x43453258,512(a5) | 'CE2X' ('STAT' follows - one check is enough)
    bne.s   hostAnswers

    move.w  520(a5),d0          | code + counter
    cmp.w   d0,d7
    beq.s   waitSecond          | not changed, nothing to print
    move.w  d0,d7

    clr.b   1023(a5)            | the text ends in the sector in any case
    pea     528(a5)
    move.w  #9,-(sp)            | Cconws()
    trap    #1
    addq.l  #6,sp
    bra.s   waitSecond

readFailed:
    subq.w  #1,d6
    beq.s   stopWaiting         | 3 in a row: the device doesn't answer (or the host refuses the ID) - boot on without it

waitSecond:
    move.l  0x4ba.w,d4          | _hz_200
    add.l   #200,d4             | 1 s, a key ends it right away
waitKey:
    move.w  #11,-(sp)           | Cconis()
    trap    #1
    addq.l  #2,sp
    tst.w   d0
    beq.s   noKey

    move.w  #8,-(sp)            | Cnecin() - take the key, ASCII in the lowest byte
    trap    #1
    addq.l  #2,sp
    cmpi.b  #0x20,d0
    beq.s   stopWaiting         | SPACE - stop waiting, other keys are ignored

noKey:
    cmp.l   0x4ba.w,d4
    bhi.s   waitKey
    bra.s   readStatus

hostAnswers:                    | connected - sector 0 in the buffer is the real root sector
    movea.l a5,a0
    moveq   #0,d0
    move.w  #255,d1
checksum:
    add.w   (a0)+,d0
    dbra    d1,checksum
    cmpi.w  #0x1234,d0
    bne.s   freeBuffer          | not executable - boot on

    move.l  a5,-(sp)            | run it like TOS would: its registers, return address to TOS on the stack
    movem.l 4(sp),d0-d7/a0-a6   | (the buffer stays allocated - the code runs from it)
    move.l  (sp),60(sp)         | the jump address over the saved a6 (restored already)
    lea     60(sp),sp
    rts

stopWaiting:
freeBuffer:
    move.l  a4,-(sp)
    move.w  #0x49,-(sp)         | Mfree()
    trap    #1
    addq.l  #6,sp
bootEnd:
    movem.l (sp)+,d0-d7/a0-a6
    rts

| Reads sectors 0 + 1 of devno to a5. Returns d0 = 0 (Z set) on success.
readSectors:
    moveq   #0,d0
    move.b  devno(pc),d0
    movea.l 0x4f2.w,a0          | _sysbase -> OS header
    cmpi.w  #0x200,2(a0)        | os_version: DMAread is there since TOS 2.00
    blo.s   directRead

    move.w  d0,-(sp)            | devno
    move.l  a5,-(sp)            | buffer
    move.w  #2,-(sp)            | count
    clr.l   -(sp)               | sector 0
    move.w  #42,-(sp)           | DMAread()
    trap    #14
    lea     14(sp),sp
    tst.l   d0
    bne.s   readErr

                                | DMA wrote to RAM behind the caches - flush them (also the instruction cache: we may run it)
    move.l  0x5a0.w,d0          | _p_cookies - find '_CPU': 20 / 30 / 40 / 60
    beq.s   flushed
    movea.l d0,a0
findCpu:
    move.l  (a0)+,d1
    beq.s   flushed             | end of the jar - 68000
    move.l  (a0)+,d0
    cmpi.l  #0x5f435055,d1
    bne.s   findCpu
    cmpi.w  #20,d0
    blo.s   flushed
    cmpi.w  #40,d0
    bhs.s   flush040
    .dc.w   0x4e7a,0x0002       | movec cacr,d0
    ori.w   #0x0808,d0          | clear data + instruction cache
    .dc.w   0x4e7b,0x0002       | movec d0,cacr
    bra.s   flushed
flush040:
    .dc.w   0xf4f8              | cpusha bc
flushed:
    moveq   #0,d0
    rts

readErr:
    moveq   #-1,d0
    rts

| direct ACSI DMA, as in ce_dd_bootsectors/bootsector_all.s - d0: devno (0..7, TOS 1.x has no SCSI)
directRead:
    lsl.b   #5,d0
    ori.b   #0x08,d0            | READ(6) of this ACSI ID
    lea     acsiCmd(pc),a0
    move.b  d0,(a0)

    lea     0xffff8604.w,a1     | DMA data register
    lea     0xffff8606.w,a2     | DMA mode register
    st      0x43e.w             | flock - lock the DMA chip

    move.l  a5,d0               | DMA address - low, mid, high
    move.b  d0,0xffff860d.w
    lsr.l   #8,d0
    move.b  d0,0xffff860b.w
    lsr.l   #8,d0
    move.b  d0,0xffff8609.w

    move.w  #0x088,(a2)         | 1st byte: NO_DMA | HDC, A1 low
    moveq   #4,d2               | cmd[0..4], then cmd[5] starts the DMA
sendCmd:
    moveq   #0,d0
    move.b  (a0)+,d0
    move.w  d0,(a1)             | the byte
    move.w  #0x08a,(a2)         | next bytes: A1 high
    moveq   #20,d1              | 0.1 s
    bsr.s   waitInt
    bne.s   dmaFail
    dbra    d2,sendCmd

    move.w  #0x190,(a2)         | toggle r/w, leave at read == clear FIFO
    move.w  #0x090,(a2)
    move.w  #2,(a1)             | DMA sector count
    move.w  #0x08a,(a2)
    move.w  #0,(a1)             | cmd[5]
    move.w  #0x00a,(a2)         | start the DMA

    moveq   #100,d1             | 1 s
    add.l   d1,d1
    bsr.s   waitInt
    bne.s   dmaFail

    move.w  #0x08a,(a2)
    move.w  (a1),d0             | status byte
    and.w   #0x00ff,d0
    beq.s   dmaEnd
dmaFail:
    moveq   #-1,d0
dmaEnd:
    move.w  #0x080,(a2)         | DMA chip back to the floppy for TOS
    sf      0x43e.w
    tst.w   d0
    rts

| d1: timeout in 200 Hz ticks. Returns Z set when the ACSI INT came.
waitInt:
    add.l   0x4ba.w,d1
wiLoop:
    btst    #5,0xfffffa01.w     | MFP GPIP bit 5 - L = INT
    beq.s   wiEnd
    cmp.l   0x4ba.w,d1
    bhi.s   wiLoop
    moveq   #-1,d1              | Z clear - timeout
wiEnd:
    rts

acsiCmd:
    .dc.b   0x08,0x00,0x00,0x00,0x02,0x00      | READ(6) sector 0, 2 sectors
