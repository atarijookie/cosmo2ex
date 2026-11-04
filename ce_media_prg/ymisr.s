| Sound on a plain ST: MFP timer A interrupt, one sample per interrupt (15754 Hz). The sample byte is an index into
| the YM table made by the host (ce_media): three longwords per entry, each written to 0xffff8800 sets one volume
| register (register number in the high word, value in the low word, which goes to 0xffff8802). The tones and the
| noise are off, so the three volumes make the output level.
|
| Made short, as it runs 15754 times a second (about 256 cycles incl. the interrupt itself - half of an 8 MHz ST):
| - the sample pointer is the address in the 'move.b' below (_ymIsrPtr), bumped with one addq
| - no end of the ring check: the ring has a spill area after it (the start of the ring copied there), and the VBL
|   routine moves the pointer back by the ring size (_ymWrap) - long before it gets to the end of the spill area
| - no check for running out of data: the VBL routine stops the timer before that (and counts the played samples
|   from the pointer)
| - the table follows the code, so it's read PC relative - no address register to set up and save

	.globl	_ymIsr
	.globl	_ymIsrPtr
	.globl	_ymTable
	.globl	_ymWrap
	.globl	_ymRingEnd
	.globl	_ymRingBytes

	.text
	.even
_ymIsr:
	move.l	d0,-(sp)
	moveq	#0,d0
	.dc.w	0x1039						| move.b (abs).l,d0 - the address is patched, so written out by hand
_ymIsrPtr:
	.dc.l	0
	addq.l	#1,_ymIsrPtr				| next sample
	lsl.w	#4,d0						| 16 B per table entry
	move.l	_ymTable(pc,d0.w),0xffff8800.w		| channel A volume
	move.l	_ymTable+4(pc,d0.w),0xffff8800.w	| channel B volume
	move.l	_ymTable+8(pc,d0.w),0xffff8800.w	| channel C volume
	move.l	(sp)+,d0
	move.b	#0xdf,0xfffffa0f.w			| MFP ISRA: timer A in-service bit off (software end of interrupt)
	rte

	.even
_ymTable:								| 256 x 16 B, copied here from GET_YM_TABLE
	.ds.b	4096

| called from the VBL routine: pointer past the ring -> back by the ring size. One sub.l on memory, so the
| interrupt can't come between reading and writing it.
_ymWrap:
	move.l	_ymIsrPtr,d0
	cmp.l	_ymRingEnd,d0
	bcs.s	1f
	move.l	_ymRingBytes,d0
	sub.l	d0,_ymIsrPtr
1:	rts

	.bss
	.even
_ymRingEnd:		.ds.l	1
_ymRingBytes:	.ds.l	1
