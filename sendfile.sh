#!/usr/bin/env bash
set -e
IP="$1"
FILE="$2"
SIZE="$(stat -c%s "$FILE")"
TARGET="$3"

(
	echo "recv '$TARGET' '$SIZE'"
	pv "$FILE" -p -t -e -a
	echo "exit"
) | socat -b524288 - TCP:"$IP":1234
