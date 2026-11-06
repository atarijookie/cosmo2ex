| Calling code which isn't built by this gcc - STinG (Pure C cdecl) and the MiNT kernel (gcc -mshort):
|   they:     arguments on stack in their real size (int16 = 2 bytes), result in d0, Pure C may destroy d0-d2/a0-a2
|   this gcc: every argument on stack as 4 bytes, result in d0, keeps d2-d7/a2-a6
| See purecall.h for the C side.

    .globl  _pc_call_0
    .globl  _pc_call_l
    .globl  _pc_call_w
    .globl  _pc_call_ll
    .globl  _pc_call_lw
    .globl  _pc_call_lll
    .globl  _pc_call_llw
    .globl  _pc_call_lllw
    .globl  _fastCopy

    .text

| ------------------------------------------------------
| gcc -> Pure C. On entry: 4(sp) fn, 8(sp) 1st arg, 12(sp) 2nd, 16(sp) 3rd.
| After saving d2/a2 (8 bytes) these are at 12, 16, 20, 24.

| int32 pc_call_0(void *fn)
_pc_call_0:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    jsr     (a0)
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_l(void *fn, int32 a) - fn(long a)
_pc_call_l:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.l  16(sp),-(sp)            | a
    jsr     (a0)
    addq.l  #4,sp
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_w(void *fn, int32 w) - fn(int16 w)
_pc_call_w:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.w  18(sp),-(sp)            | lower word of w
    jsr     (a0)
    addq.l  #2,sp
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_lw(void *fn, int32 a, int32 w) - fn(long a, int16 w)
_pc_call_lw:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.w  22(sp),-(sp)            | lower word of w (at 20)
    move.l  18(sp),-(sp)            | a (at 16, +2 for the word above)
    jsr     (a0)
    addq.l  #6,sp
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_ll(void *fn, int32 a, int32 b) - fn(long a, long b)
_pc_call_ll:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.l  20(sp),-(sp)            | b (at 20)
    move.l  20(sp),-(sp)            | a (at 16, +4)
    jsr     (a0)
    addq.l  #8,sp
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_llw(void *fn, int32 a, int32 b, int32 w) - fn(long a, long b, int16 w)
_pc_call_llw:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.w  26(sp),-(sp)            | lower word of w (at 24)
    move.l  22(sp),-(sp)            | b (at 20, +2)
    move.l  22(sp),-(sp)            | a (at 16, +6)
    jsr     (a0)
    lea     10(sp),sp
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_lllw(void *fn, int32 a, int32 b, int32 c, int32 w) - fn(long a, long b, long c, int16 w)
_pc_call_lllw:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.w  30(sp),-(sp)            | lower word of w (at 28)
    move.l  26(sp),-(sp)            | c (at 24, +2)
    move.l  26(sp),-(sp)            | b (at 20, +6)
    move.l  26(sp),-(sp)            | a (at 16, +10)
    jsr     (a0)
    lea     14(sp),sp
    movem.l (sp)+,d2/a2
    rts

| int32 pc_call_lll(void *fn, int32 a, int32 b, int32 c) - fn(long a, long b, long c)
_pc_call_lll:
    movem.l d2/a2,-(sp)
    move.l  12(sp),a0
    move.l  24(sp),-(sp)            | c (at 24)
    move.l  24(sp),-(sp)            | b (at 20, +4)
    move.l  24(sp),-(sp)            | a (at 16, +8)
    jsr     (a0)
    lea     12(sp),sp
    movem.l (sp)+,d2/a2
    rts

| ------------------------------------------------------
| void fastCopy(void *dst, const void *src, uint32 len) - long moves when both addresses are even, bytes otherwise
_fastCopy:
    move.l  4(sp),a0                | dst
    move.l  8(sp),a1                | src
    move.l  12(sp),d0               | len

    move.w  a0,d1
    btst    #0,d1
    bne.s   fcBytes
    move.w  a1,d1
    btst    #0,d1
    bne.s   fcBytes

    move.l  d0,d1
    lsr.l   #4,d1                   | 16 byte blocks
    and.l   #15,d0                  | what stays for after them
    bra.s   fcBlockNext
fcBlock:
    move.l  (a1)+,(a0)+
    move.l  (a1)+,(a0)+
    move.l  (a1)+,(a0)+
    move.l  (a1)+,(a0)+
fcBlockNext:
    subq.l  #1,d1
    bpl.s   fcBlock

fcBytes:
    subq.l  #1,d0
    bmi.s   fcDone
    move.b  (a1)+,(a0)+
    bra.s   fcBytes
fcDone:
    rts
