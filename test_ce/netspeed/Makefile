# netspeed.ttp - MiNT program, uses mintlib's sockets. See README.md
CC      = m68k-atari-mint-gcc
STRIP   = m68k-atari-mint-strip
FLAGS   = m68k-atari-mint-flags
CFLAGS  = -std=gnu99 -O2 -m68000 -Wall
TARGET  = netspeed.ttp

all: $(TARGET)

$(TARGET): netspeed.c
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) -s $@
	$(FLAGS) --mfastload --mfastram --mfastalloc $@

# the same source for the PC, to test the program and the server without an Atari
netspeed_host: netspeed.c
	gcc -std=gnu99 -O2 -Wall -o $@ $<

clean:
	$(RM) $(TARGET) netspeed_host

.PHONY: all clean
