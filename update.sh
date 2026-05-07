#!/usr/bin/env bash
set -e
IP="$1"
ELF="mc0:/APPS/ps2netcon.elf"

make
./sendfile.sh "$IP" ps2netcon.elf "$ELF"
echo "elf '$ELF'" | socat -,ignoreeof TCP:"$IP":1234
