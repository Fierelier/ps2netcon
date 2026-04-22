#!/usr/bin/env bash
set -e
IP="$1"
FILE="$2"
SIZE="$(stat -c%s "$FILE")"
TARGET="$3"

(
	echo "recv '$TARGET' '$SIZE'"
	pv "$FILE"
	echo "exit"
) | nc "$IP" 1234
