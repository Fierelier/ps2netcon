#!/usr/bin/env bash
set -e
IP="$1"
FILE="$2"
SIZE="$(stat -c%s "$FILE")"
TARGET="$3"
TARGET="${TARGET//\\/\\\\}" # replace \ with \\
TARGET="${TARGET//\'/\\\'}" # replace ' with \'

(
	echo "recv '$TARGET' '$SIZE'"
	pv "$FILE" --no-splice -p -t -e -a
	echo "exit"
) | socat -b524288 -,ignoreeof TCP:"$IP":1234
