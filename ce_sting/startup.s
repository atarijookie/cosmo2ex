| Startup of CE_ETH.STX - STinG starts it as a program, main() stays resident with Ptermres() when everything is fine.

    .globl  _main
    .globl  __base
    .globl  ___main

| ------------------------------------------------------
    .text

    move.l  4(sp),a5                | address to basepage
    move.l  a5,__base
    move.l  0x0c(a5),d0             | length of text segment
    add.l   0x14(a5),d0             | length of data segment
    add.l   0x1c(a5),d0             | length of bss segment
    add.l   #0x1000+0x100,d0        | length of stack+basepage
    move.l  a5,d1                   | address to basepage
    add.l   d0,d1                   | end of program
    and.b   #0xf0,d1                | align stack
    move.l  d1,sp                   | new stackspace

    move.l  d0,-(sp)                | Mshrink()
    move.l  a5,-(sp)
    clr.w   -(sp)
    move.w  #0x4a,-(sp)
    trap    #1
    lea.l   12(sp),sp

    jsr     _main                   | returns only when not installed

    move.w  d0,-(sp)                | Pterm(d0)
    move.w  #0x4c,-(sp)
    trap    #1

___main:
    rts

| ------------------------------------------------------
    .bss
__base:     .ds.l   1
