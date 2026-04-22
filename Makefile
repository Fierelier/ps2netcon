# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.

EE_BIN = ps2netcon.elf
EE_OBJS = main.o ps2ips_irx.o
EE_LIBS = -lps2ips -lc
EE_CFLAGS = -Os
EE_LDFLAGS = -s

all: ps2ips_irx.c $(EE_BIN)

ps2ips_irx.c: $(PS2SDK)/iop/irx/ps2ips.irx
	bin2c $< ps2ips_irx.c ps2ips_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) ps2ips_irx.c

run: $(EE_BIN)
	ps2client execee host:$(EE_BIN)

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
