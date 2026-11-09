| Built-in floppy of ce_pico_fdd: shows the device status on the ST until a key is pressed.
|
| Assembled twice by make_builtin.py:
|   BOOT=1  executable boot sector code (goes to offset 0x1e of the boot sector, max 480 bytes)
|   BOOT=0  CE_FDD.TOS, a TOS program
| Both are raw position independent code, no relocation.
|
| Once a second it reads the status sector (track 0, side 0, sector 10 - the BPB says 9 sectors per track, so GEMDOS never
| uses it) which the device regenerates when its status changes:
|   0..7    'CE2XSTAT'
|   8       status code
|   9       change counter - the text is printed again when this or the code changes
|   16..    text to print (VT52), 0 terminated
| When the sector isn't there (read error or no magic) 2 times in a row, the device switched to the real image - the
| text 'ready, reset' is printed then. It keeps checking until SPACE is pressed - other keys are ignored, so a key pressed
| by chance doesn't stop the waiting.

    .text

.if BOOT

| boot sector: runs in supervisor mode from the disk buffer, GEMDOS is up already. Read buffer from Malloc().
boot:
    movem.l d0-d7/a0-a6,-(sp)

    move.l  #512,-(sp)
    move.w  #0x48,-(sp)         | Malloc()
    trap    #1
    addq.l  #6,sp
    tst.l   d0
    beq.s   bootEnd             | no memory? just boot on
    movea.l d0,a5

    bsr.s   statusLoop

    move.l  a5,-(sp)
    move.w  #0x49,-(sp)         | Mfree()
    trap    #1
    addq.l  #6,sp

bootEnd:
    movem.l (sp)+,d0-d7/a0-a6
    rts

.else

| TOS program: the whole TPA is ours, the read buffer goes on the stack.
start:
    lea     -512(sp),sp
    movea.l sp,a5

    bsr.s   statusLoop

    clr.w   -(sp)               | Pterm0()
    trap    #1

.endif

| a5: 512 B buffer. Returns after SPACE was pressed.
statusLoop:
    moveq   #-1,d7              | code + counter shown now, -1 nothing, -2 the 'ready' text
    moveq   #0,d6               | reads in a row without the status sector

readStatus:
    move.w  #1,-(sp)            | count
    clr.w   -(sp)               | side 0
    clr.w   -(sp)               | track 0
    move.w  #10,-(sp)           | sector 10
    clr.w   -(sp)               | drive A
    clr.l   -(sp)               | filler
    move.l  a5,-(sp)            | buffer
    move.w  #8,-(sp)            | Floprd()
    trap    #14
    lea     20(sp),sp

    tst.w   d0
    bne.s   notOurs
    cmpi.l  #0x43453258,(a5)    | 'CE2X'
    bne.s   notOurs
    cmpi.l  #0x53544154,4(a5)   | 'STAT'
    bne.s   notOurs

    moveq   #0,d6
    move.w  8(a5),d0            | code + counter
    cmp.w   d0,d7
    beq.s   waitSecond          | not changed, nothing to print
    move.w  d0,d7

    pea     clearScreen(pc)
    move.w  #9,-(sp)            | Cconws()
    trap    #1
    addq.l  #6,sp

    clr.b   511(a5)             | the text ends in the sector in any case
    pea     16(a5)
    move.w  #9,-(sp)            | Cconws()
    trap    #1
    addq.l  #6,sp
    bra.s   waitSecond

notOurs:
    addq.w  #1,d6
    cmpi.w  #2,d6
    blo.s   waitSecond          | maybe just a bad read, try again
    cmpi.w  #-2,d7
    beq.s   waitSecond          | shown already
    moveq   #-2,d7

    pea     readyText(pc)
    move.w  #9,-(sp)            | Cconws()
    trap    #1
    addq.l  #6,sp

waitSecond:
    moveq   #49,d5              | 50 VBLs, a key ends it right away
waitVbl:
    move.w  #37,-(sp)           | Vsync()
    trap    #14
    addq.l  #2,sp

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
    dbra    d5,waitVbl
    bra     readStatus

stopWaiting:
    pea     cursorOn(pc)
    move.w  #9,-(sp)            | Cconws()
    trap    #1
    addq.l  #6,sp
    rts

clearScreen:
    .asciz  "\033E\033f"
readyText:
    .ascii  "\033E\033f\r\n Cosmo2ex floppy\r\n ---------------\r\n\r\n"
    .ascii  " The floppy image is ready.\r\n\r\n"
    .ascii  " Reset your ST to boot from it.\r\n\r\n"
    .asciz  " (SPACE: continue without the reset)\r\n"
cursorOn:
    .asciz  "\033e"
    .even
