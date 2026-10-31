SLIM	= yes

CC	= m68k-atari-mint-gcc
STRIP	= m68k-atari-mint-strip
CFLAGS	= -O2 -m68000 -fomit-frame-pointer -Wall -Wno-multichar -mpcrel -nostdlib -DSLIM
LDFLAGS	= -Wl,--traditional-format

HOSTCXX = g++
CXXFLAGS = -Wall

$(TARGET): $(STARTUP) $(SSRC) 
	$(CC) $(CFLAGS) -o $@ $(STARTUP) $(CSRC) $(SSRC) $(LDFLAGS)
	$(STRIP) -s $@

# copied to the config drive in the source tree too, see CONFIGDRIVE_PRG in ../ce_dd_prg/Makefile
CONFIGDRIVE = ../ce_cores/configdrive/drivers

all:	ce_dd.bs ce_dd_l2.bs $(CONFIGDRIVE)/ce_dd.bs $(CONFIGDRIVE)/ce_dd_l2.bs

ce_dd_l2.bs:	ce_dd.bs		# both made by convertor

$(CONFIGDRIVE)/%.bs:	%.bs
	mkdir -p $(CONFIGDRIVE)
	cp $< $@.tmp && mv -f $@.tmp $@

convertor:	convertor.cpp
	$(HOSTCXX) $(CXXFLAGS) -o $@ $<

convertor:	bs.prg bs_l2.prg ../ce_dd_prg/ce_dd.prg

../ce_dd_prg/ce_dd.prg:	../ce_dd_prg/main.c
	$(MAKE) -C ../ce_dd_prg

ce_dd.bs:	bs.prg
	$(MAKE) convertor
	./convertor

bs.prg: bootsector_all.s
	$(CC) $(CFLAGS) -o $@ bootsector_all.s $(LDFLAGS)
	$(STRIP) -s $@
    
bs_l2.prg: bootsector_level2.s
	$(CC) $(CFLAGS) -o $@ bootsector_level2.s $(LDFLAGS)
	$(STRIP) -s $@
    
clean:
	$(RM) *.prg *.bs *~ *.o convertor
