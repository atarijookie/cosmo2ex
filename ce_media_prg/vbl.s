| VBL interrupt handler used instead of the one of TOS while the player has the DMA sound (STE / TT).
|
| 'DMA sound plays' is XORed into MFP GPIP 7, the mono monitor detect. The VBL handler of TOS compares that bit with
| the video mode on every VBL, and when they don't match it switches the video mode to the other monitor type and
| calls swv_vec (by default: reset). So with DMA sound playing, TOS reset the TT, or (swv_vec ignored) switched the
| screen to the wrong mode - dark monitor, garbage on the desktop afterwards.
|
| This one does only what's needed while playing: the frame counters (Vsync() waits for _frclock) and the VBL queue
| (the floppy's routine and the player's). No monitor check, no cursor blink, no colorptr / screenpt (the player
| doesn't use Setpalette / Setscreen while it runs).

	.globl	_vblHandler

	.text
	.even
_vblHandler:
	addq.l	#1,0x466.w					| _frclock
	subq.w	#1,0x452.w					| vblsem - not inside another VBL
	bmi.s	9f
	movem.l	d0-d7/a0-a6,-(sp)
	addq.l	#1,0x462.w					| _vbclock

	move.w	0x454.w,d7					| nvbls
	beq.s	8f
	subq.w	#1,d7
	move.l	0x456.w,a0					| _vblqueue
1:	move.l	(a0)+,d0
	beq.s	2f
	move.l	a0,-(sp)					| the routines may change any register
	move.w	d7,-(sp)
	move.l	d0,a1
	jsr		(a1)
	move.w	(sp)+,d7
	move.l	(sp)+,a0
2:	dbra	d7,1b

8:	movem.l	(sp)+,d0-d7/a0-a6
9:	addq.w	#1,0x452.w
	rte
