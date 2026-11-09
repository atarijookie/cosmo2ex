| FDDOFF.PRG - TEMPORARY workaround, remove once SLM printing is tested
|
| On a machine without a floppy drive the floppy controller never gets index pulses, so its motor-on bit never clears
| and TOS never deselects a drive once it was selected (by any floppy access) - the drive select lines (PSG port A
| bits 1 and 2, active low) stay active. Papyrus (and probably Calamus) check these lines before using the DMA port
| and refuse to print with 'die Floppy ist noch aktiv'.
|
| This program stays resident: a VBL routine deselects both drives whenever one of them is selected, and counts how
| many times it had to do it. Run it again to see the counter (it's not installed twice). It also shows the select
| lines and the floppy controller status (motor-on bit) - as a hint for programs which might check the controller
| instead of the select lines.
|
| Don't use it with a floppy drive connected - it would deselect the drive in the middle of floppy access.

    .text
    .globl  _start
_start:
    move.l  4(%sp),%a0              | basepage
    lea     basepage(%pc),%a1
    move.l  %a0,(%a1)

    pea     super(%pc)
    move.w  #38,-(%sp)              | Supexec()
    trap    #14
    addq.l  #6,%sp

    pea     msgTitle(%pc)
    bsr     print

    pea     msgSelect(%pc)
    bsr     print
    move.b  selNow(%pc),%d0
    bsr     printSelect

    pea     msgFdc(%pc)
    bsr     print
    lea     fdcRead(%pc),%a0
    tst.b   (%a0)
    bne.s   1f
    pea     msgFdcLocked(%pc)       | DMA in use, status not read
    bsr     print
    bra.s   2f
1:  move.b  fdcStatus(%pc),%d0
    move.b  %d0,-(%sp)
    bsr     printHex
    move.b  (%sp)+,%d0
    lea     msgMotorOn(%pc),%a0
    btst    #7,%d0
    bne.s   3f
    lea     msgMotorOff(%pc),%a0
3:  move.l  %a0,-(%sp)
    bsr     print
2:
    lea     state(%pc),%a0
    move.b  (%a0),%d0
    cmpi.b  #STATE_ALREADY,%d0
    beq.s   showCount
    cmpi.b  #STATE_INSTALLED,%d0
    beq.s   installed

    pea     msgNoSlot(%pc)          | VBL queue full
    bsr     print
    bra.s   endNotResident

showCount:
    pea     msgAlready(%pc)
    bsr     print
    move.l  foundCount(%pc),%d0
    bsr     printHexLong
    pea     msgTimes(%pc)
    bsr     print
    bra.s   endNotResident

installed:
    pea     msgInstalled(%pc)
    bsr     print
    bsr     waitKey

    move.l  basepage(%pc),%a0       | keep basepage + text + data + bss
    move.l  12(%a0),%d0             | p_tlen
    add.l   20(%a0),%d0             | p_dlen
    add.l   28(%a0),%d0             | p_blen
    addi.l  #256,%d0
    clr.w   -(%sp)
    move.l  %d0,-(%sp)
    move.w  #49,-(%sp)              | Ptermres()
    trap    #1

endNotResident:
    bsr     waitKey
    clr.w   -(%sp)                  | Pterm0()
    trap    #1

waitKey:
    pea     msgEnd(%pc)
    bsr     print
    move.w  #7,-(%sp)               | Crawcin() - wait for a key, so it can be read when started from the desktop
    trap    #1
    addq.l  #2,%sp
    rts

|---------------------------------------
| the resident VBL routine - TOS saves all registers before calling the VBL queue routines
| (the magic and the counter are just before it - a second run finds them through the VBL queue)
    .even
residentMagic:
    .ascii  "FDOF"
deselCount:
    .long   0
vblHandler:
    move.w  %sr,-(%sp)
    ori.w   #0x0700,%sr             | no interrupts - TOS uses the PSG from interrupts too
    move.b  #14,0xffff8800.w        | select PSG register 14 - port A
    move.b  0xffff8800.w,%d0
    move.b  %d0,%d1
    andi.b  #0x06,%d1
    cmpi.b  #0x06,%d1
    beq.s   1f                      | both drives deselected? nothing to do
    ori.b   #0x06,%d0               | bits 1 (drive A) and 2 (drive B) to H = deselected
    move.b  %d0,0xffff8802.w
    lea     deselCount(%pc),%a0
    addq.l  #1,(%a0)
1:  move.w  (%sp)+,%sr
    rts

|---------------------------------------
| supervisor part of the start
STATE_NO_SLOT   = 0
STATE_INSTALLED = 1
STATE_ALREADY   = 2

super:
    move.w  %sr,-(%sp)
    ori.w   #0x0700,%sr
    move.b  #14,0xffff8800.w
    move.b  0xffff8800.w,%d0
    lea     selNow(%pc),%a0
    move.b  %d0,(%a0)
    move.w  (%sp)+,%sr

    tst.w   0x43e.w                 | _flock - DMA in use by someone else? don't touch the controller
    bne.s   1f
    st      0x43e.w
    move.w  #0x80,0xffff8606.w      | DMA mode: floppy controller, status register
    move.w  0xffff8604.w,%d0        | read the status
    lea     fdcStatus(%pc),%a0
    move.b  %d0,(%a0)
    st      fdcRead-fdcStatus(%a0)
    clr.w   0x43e.w
1:
    | already installed? look for the magic before the routines in the VBL queue
    move.w  0x454.w,%d1             | nvbls
    subq.w  #1,%d1
    bmi.s   noSlot
    move.l  0x456.w,%a1             | _vblqueue
    move.l  #0x46444f46,%d2         | 'FDOF'
2:  move.l  (%a1)+,%d0
    beq.s   3f
    move.l  %d0,%a0
    cmp.l   -8(%a0),%d2
    bne.s   3f
    lea     foundCount(%pc),%a2
    move.l  -4(%a0),(%a2)
    lea     state(%pc),%a2
    move.b  #STATE_ALREADY,(%a2)
    rts
3:  dbf     %d1,2b

    | not installed - put it into the first free slot
    move.w  0x454.w,%d1
    subq.w  #1,%d1
    move.l  0x456.w,%a1
4:  tst.l   (%a1)+
    beq.s   5f
    dbf     %d1,4b
noSlot:
    lea     state(%pc),%a2
    move.b  #STATE_NO_SLOT,(%a2)
    rts
5:  lea     vblHandler(%pc),%a0
    move.l  %a0,-(%a1)
    lea     state(%pc),%a2
    move.b  #STATE_INSTALLED,(%a2)
    rts

|---------------------------------------
| print the drive select bits in d0
printSelect:
    move.b  %d0,-(%sp)
    bsr     printHex
    pea     msgDriveA(%pc)
    bsr     print
    move.b  (%sp),%d0
    lea     msgSelected(%pc),%a0
    btst    #1,%d0
    beq.s   1f
    lea     msgDeselected(%pc),%a0
1:  move.l  %a0,-(%sp)
    bsr     print
    pea     msgDriveB(%pc)
    bsr     print
    move.b  (%sp)+,%d0
    lea     msgSelected(%pc),%a0
    btst    #2,%d0
    beq.s   2f
    lea     msgDeselected(%pc),%a0
2:  move.l  %a0,-(%sp)
    bsr     print
    pea     msgNewLine(%pc)
    bsr     print
    rts

| print d0.l as 8 hex digits
printHexLong:
    moveq   #3,%d2
1:  rol.l   #8,%d0
    movem.l %d0/%d2,-(%sp)
    bsr     printHex
    movem.l (%sp)+,%d0/%d2
    dbf     %d2,1b
    rts

| print d0.b as 2 hex digits
printHex:
    lea     hexBuf(%pc),%a0
    move.b  %d0,%d1
    lsr.b   #4,%d1
    bsr.s   1f
    move.b  %d0,%d1
    bsr.s   1f
    clr.b   (%a0)
    pea     hexBuf(%pc)
    bsr     print
    rts
1:  andi.w  #0x0f,%d1
    move.b  hexDigits(%pc,%d1.w),(%a0)+
    rts
hexDigits:
    .ascii  "0123456789ABCDEF"

| print the string, its address is on stack (removed by this function)
print:
    move.l  4(%sp),-(%sp)
    move.w  #9,-(%sp)               | Cconws()
    trap    #1
    addq.l  #6,%sp
    move.l  (%sp)+,(%sp)            | return address over the argument
    rts

|---------------------------------------
    .data
basepage:       .long 0
foundCount:     .long 0
selNow:         .byte 0
fdcRead:        .byte 0
fdcStatus:      .byte 0
state:          .byte 0
hexBuf:         .byte 0,0,0,0

msgTitle:       .asciz "\r\nFDDOFF - keep floppy drives deselected (temporary workaround)\r\n\n"
msgSelect:      .asciz "PSG port A: $"
msgDriveA:      .asciz "  drive A: "
msgDriveB:      .asciz ", drive B: "
msgSelected:    .asciz "selected"
msgDeselected:  .asciz "deselected"
msgNewLine:     .asciz "\r\n"
msgFdc:         .asciz "Floppy controller status: $"
msgFdcLocked:   .asciz "not read (DMA in use)"
msgMotorOn:     .asciz "  motor ON\r\n"
msgMotorOff:    .asciz "  motor off\r\n"
msgInstalled:   .asciz "\r\nInstalled - drives are kept deselected from now on.\r\n"
msgAlready:     .asciz "\r\nAlready installed, it had to deselect a drive $"
msgTimes:       .asciz " times.\r\n"
msgNoSlot:      .asciz "\r\nNot installed - no free slot in VBL queue!\r\n"
msgEnd:         .asciz "\r\nPress any key.\r\n"
    .even
