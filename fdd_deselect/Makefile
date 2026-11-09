# FDDOFF.PRG - TEMPORARY workaround for testing SLM printing on a machine without floppy drive, see fddoff.S.
# Remove this directory once Papyrus / Calamus printing works.
# The result is also copied to the config drive (ce_cores/configdrive/tests/), so it can be run from there.

CC = m68k-atari-mint-gcc

all: FDDOFF.PRG

FDDOFF.PRG: fddoff.S
	$(CC) -nostdlib -nostartfiles -o $@ $<
	m68k-atari-mint-strip $@
	mkdir -p ../ce_cores/configdrive/tests
	cp $@ ../ce_cores/configdrive/tests/

clean:
	rm -f FDDOFF.PRG ../ce_cores/configdrive/tests/FDDOFF.PRG

.PHONY: all clean
