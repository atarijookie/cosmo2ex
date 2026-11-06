| Entries for the kernel (gcc -mshort: int16 arguments are 2 bytes on stack) into this gcc code, which takes all arguments
| as 4 bytes. The calls in the other direction are in ../ce_sting_stx_driver/purecall.s. See mint.h.

    .globl  _pc_xif_output
    .globl  _pc_xif_ioctl

    .globl  _xifOutput
    .globl  _xifIoctl

    .text

| long output(struct netif *nif, BUF *buf, const char *hwaddr, short hwlen, short pktype):
| 4(sp) nif, 8(sp) buf, 12(sp) hwaddr, 16(sp) hwlen, 18(sp) pktype
_pc_xif_output:
    moveq   #0,d0
    move.w  18(sp),d0
    move.l  d0,-(sp)                | pktype, unsigned - RARP is 0x8035
    moveq   #0,d0
    move.w  20(sp),d0               | hwlen (at 16, +4)
    move.l  d0,-(sp)
    move.l  20(sp),-(sp)            | hwaddr (at 12, +8)
    move.l  20(sp),-(sp)            | buf (at 8, +12)
    move.l  20(sp),-(sp)            | nif (at 4, +16)
    jsr     _xifOutput
    lea     20(sp),sp
    rts

| long ioctl(struct netif *nif, short cmd, long arg): 4(sp) nif, 8(sp) cmd, 10(sp) arg
_pc_xif_ioctl:
    move.l  10(sp),-(sp)            | arg
    moveq   #0,d0
    move.w  12(sp),d0               | cmd (at 8, +4)
    move.l  d0,-(sp)
    move.l  12(sp),-(sp)            | nif (at 4, +8)
    jsr     _xifIoctl
    lea     12(sp),sp
    rts
