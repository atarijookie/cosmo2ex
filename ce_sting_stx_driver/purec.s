| Entries for STinG (Pure C cdecl) into this gcc code, which takes all arguments as 4 bytes - see purecall.s for the calls
| in the other direction, and sting.h.

    .globl  _pc_set_state
    .globl  _pc_cntrl

    .globl  _my_set_state
    .globl  _my_cntrl

    .text

| ------------------------------------------------------
| Pure C -> gcc. gcc keeps more registers than Pure C needs, so only the int16 arguments need widening.

| int16 cdecl set_state(PORT *port, int16 enable): 4(sp) port, 8(sp) enable
_pc_set_state:
    move.w  8(sp),d0
    ext.l   d0
    move.l  d0,-(sp)                | enable
    move.l  8(sp),-(sp)             | port (at 4, +4)
    jsr     _my_set_state
    addq.l  #8,sp
    rts

| int16 cdecl cntrl(PORT *port, uint32 arg, int16 code): 4(sp) port, 8(sp) arg, 12(sp) code
_pc_cntrl:
    move.w  12(sp),d0
    ext.l   d0
    move.l  d0,-(sp)                | code
    move.l  12(sp),-(sp)            | arg (at 8, +4)
    move.l  12(sp),-(sp)            | port (at 4, +8)
    jsr     _my_cntrl
    lea     12(sp),sp
    rts
