#!/usr/bin/env bash
set -e
IP="$1"
NEUTRINO="mc0:/APPS/neutrino/modules"

(
	echo "irx $NEUTRINO/iomanX.irx"
#	echo "irx $NEUTRINO/fileXio.irx"
	echo "irx $NEUTRINO/bdm.irx"
	echo "irx $NEUTRINO/bdmfs_fatfs.irx"
	echo "irx $NEUTRINO/ata_bd.irx"
	echo "exit"
) | socat - TCP:"$IP":1234
