# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.

EE_BIN = ps2netcon_uncompressed.elf
EE_BIN_COMPRESSED = ps2netcon.elf
EE_OBJS = main.o DEV9_irx.o NETMAN_irx.o SMAP_irx.o SIO2MAN_irx.o MCMAN_irx.o MCSERV_irx.o FILEIO_irx.o loader_elf.o
EE_LIBS = -lc -lnetman -lps2ip -ldebug -lpatches
EE_CFLAGS = -Os
EE_LDFLAGS = -s

all: DEV9_irx.c NETMAN_irx.c SMAP_irx.c loader/loader.elf loader_elf.c $(EE_BIN) $(EE_BIN_COMPRESSED)

DEV9_irx.c: $(PS2SDK)/iop/irx/ps2dev9.irx
	bin2c $< DEV9_irx.c DEV9_irx

NETMAN_irx.c: $(PS2SDK)/iop/irx/netman.irx
	bin2c $< NETMAN_irx.c NETMAN_irx

SMAP_irx.c: $(PS2SDK)/iop/irx/smap.irx
	bin2c $< SMAP_irx.c SMAP_irx

SIO2MAN_irx.c: $(PS2SDK)/iop/irx/sio2man.irx
	bin2c $< SIO2MAN_irx.c SIO2MAN_irx

MCMAN_irx.c: $(PS2SDK)/iop/irx/mcman.irx
	bin2c $< MCMAN_irx.c MCMAN_irx

MCSERV_irx.c: $(PS2SDK)/iop/irx/mcserv.irx
	bin2c $< MCSERV_irx.c MCSERV_irx

FILEIO_irx.c: $(PS2SDK)/iop/irx/fileXio.irx
	bin2c $< FILEIO_irx.c FILEIO_irx

loader/loader.elf: loader/Makefile
	$(MAKE) -C loader

loader_elf.c: loader/loader.elf
	bin2c loader/loader.elf loader_elf.c loader_elf

ps2netcon.elf: $(EE_BIN)
	ps2-packer-lite $(EE_BIN) $(EE_BIN_COMPRESSED)

clean:
	rm -f $(EE_BIN) $(EE_BIN_COMPRESSED) $(EE_OBJS) *_irx.c *_elf.c
	$(MAKE) -C loader clean

run: $(EE_BIN_COMPRESSED)
	ps2client execee host:$(EE_BIN_COMPRESSED)

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
