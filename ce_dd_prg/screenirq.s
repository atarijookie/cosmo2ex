| some helper routines to handle the VBL and calling of screencast / screenshot worker routines

    .globl  _init_screencapture
    .globl  _screenVblTick
    .globl  _screenworker

    .text

_vblqueue   = 0x456 |.l
_nvbls      = 0x454 |.w
_flock      = 0x43e |.w
_v_bas_ad   = 0x44e |.l

_init_screencapture:
    movem.l D0-A6,-(SP)

    move.l  0x70,__savevbl
    move.l  #screenirq,0x70                     | install VBL

    movem.l (SP)+,D0-A6
    rts

| -------------------------------------------------
screenirq:
    movem.l D0-A6,-(SP)

    move.l  SP,__savesp                         | the workers run on our own stack, not on the stack of whatever the VBL interrupted
    lea     __privstacktop,SP

    | --------------------
    | C code counts VBLs and tells if it's time to poll the host or send a screen
    jsr     _screenVblTick
    tst.b   D0
    beq.s   screenirq_end                       | nothing to do on this VBL

    | --------------------
    | try to acquire FLOCK if it's not set, and if it's used by someone else, skip this cycle
    move.w  _flock,-(SP)                        | save flock

    move.w  sr,-(SP)
    move    #0x2700,sr                          | protect this from interrupts, making it almost atomic

    tst.w   _flock                              | unfortunately we can't use tas, as the _flock word is perused by different drivers differently - TOS sets 1.w, other only st the first byte etc.
    bne.s   castskipflock                       | don't do anything if flock is set - we can't use the DMA then, anyways

    move.w  #-1,_flock                          | keep others from using DMA
    move.w  (SP)+,sr
    | --------------------

    jsr     _screenworker                       | call worker

    move.w  (SP)+,_flock                        | set flock to previous state
    bra.s   screenirq_end

castskipflock:
    move.w  (SP)+,sr
    move.w  (SP)+,_flock                        | set flock to previous state

screenirq_end:
    move.l  __savesp,SP                         | back to the interrupted stack
    movem.l (SP)+,D0-A6
    move.l  __savevbl,-(SP)                     | call original VBL routine
    rts
| -------------------------------------------------

    .bss
__savevbl:              .ds.l   1
__savesp:               .ds.l   1           | SP of interrupted code while the workers run on private stack
__privstack:            .ds.b   2048        | private stack for the workers - VBL doesn't nest (runs at IPL 4), so one is enough
__privstacktop:
