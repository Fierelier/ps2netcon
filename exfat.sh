#!/usr/bin/env bash
set -e
IP="$1"

(
	echo "irx host:/neutrino/modules/iomanX.irx"
	echo "irx host:/neutrino/modules/fileXio.irx"
	echo "irx host:/neutrino/modules/bdm.irx"
	echo "irx host:/neutrino/modules/bdmfs_fatfs.irx"
	echo "irx host:/neutrino/modules/ata_bd.irx"
	echo "exit"
) | nc "$IP" 1234
