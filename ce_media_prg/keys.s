| Raw keyboard for CE_DOOM.PRG: replaces ikbdsys (Kbdvbase()->ikbdsys) while the game runs. TOS's keyboard / MIDI
| ACIA interrupt calls it; it takes the bytes from the keyboard ACIA - key presses (scancode) and releases
| (scancode | 0x80) - into a ring the main loop sends to the host. IKBD packets (mouse, joystick, clock, status)
| are skipped. TOS doesn't see the keys meanwhile (no key click, nothing piles up in its buffer).

	.globl	_keyIsr
	.globl	_keyRing
	.globl	_keyHead

ACIA_CTRL	= 0xfffffc00
ACIA_DATA	= 0xfffffc02

	.text
	.even
_keyIsr:
	movem.l	d0-d1/a0,-(sp)
1:	move.b	ACIA_CTRL,d0
	btst	#0,d0						| receive register full?
	bne.s	2f
	btst	#5,d0						| overrun without data - reading the data register clears it
	beq.s	9f
	move.b	ACIA_DATA,d0
	bra.s	9f

2:	move.b	ACIA_DATA,d0
	tst.b	skip						| inside a packet - its rest
	beq.s	3f
	subq.b	#1,skip
	bra.s	1b

3:	cmp.b	#0xf6,d0
	bcs.s	4f							| below 0xf6: a key
	moveq	#0,d1
	move.b	d0,d1
	sub.b	#0xf6,d1
	lea		packetLen(pc),a0
	move.b	(a0,d1.w),skip
	bra.s	1b

4:	moveq	#0,d1
	move.b	_keyHead,d1
	lea		_keyRing,a0
	move.b	d0,(a0,d1.w)
	addq.b	#1,d1
	and.b	#63,d1
	move.b	d1,_keyHead					| a full ring overwrites the oldest - the main loop is quick enough
	bra.s	1b

9:	movem.l	(sp)+,d0-d1/a0
	rts

| bytes after the header 0xf6 .. 0xff: status, abs. mouse, rel. mouse x4, time of day, joystick report, joystick 0 / 1
packetLen:
	.byte	7, 5, 2, 2, 2, 2, 6, 2, 1, 1
	.even

	.bss
	.even
_keyRing:	.ds.b	64
_keyHead:	.ds.b	1
skip:		.ds.b	1
