#!/bin/sh
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"

STDGUI="${STDGUI:-$HERE/../../stdgui}"
UNISOCKET_INC="${UNISOCKET_INC:-$HERE/../..}"
OUT="${OUT:-$HERE/bin}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra -Wno-format-truncation}"
SRC="main.c platform.c zipfs.c ps2.c drivers.c"

target="$1"
if [ -z "$target" ]; then
	target=unix
	if [ -n "$WINDIR" ]; then target=win32; fi
fi

run() {
	echo "$@"
	"$@"
}

case "$target" in
unix)
	CC="${CC:-cc}"
	mkdir -p "$OUT"
	run $CC $CFLAGS -I"$STDGUI/src" -I"$UNISOCKET_INC" $SRC "$STDGUI/src/stdgui.c" "$STDGUI/src/stdgui_x11.c" -o "$OUT/ps2netcon_gui" -lX11
	;;
win32)
	if [ -n "$WINDIR" ]; then WINCC="${WINCC:-${CC:-gcc}}"; fi
	WINCC="${WINCC:-i686-w64-mingw32-gcc}"
	W95="-m32 -march=i486 -D_WIN32_WINNT=0x0400 -DWINVER=0x0400 -D__USE_MINGW_ANSI_STDIO=0"
	W95LD="-Wl,--major-os-version=4,--minor-os-version=0,--major-subsystem-version=4,--minor-subsystem-version=0"
	WINRES="${WINRES:-i686-w64-mingw32-windres}"
	mkdir -p "$OUT"
	RES=""
	if command -v $WINRES >/dev/null 2>&1; then
		run $WINRES -i "$STDGUI/src/stdgui.rc" -o "$OUT/stdgui_res.o" --include-dir "$STDGUI/src"
		RES="$OUT/stdgui_res.o"
	fi
	run $WINCC $CFLAGS $W95 $W95LD -mwindows -I"$STDGUI/src" -I"$UNISOCKET_INC" $SRC "$STDGUI/src/stdgui.c" "$STDGUI/src/stdgui_win32.c" $RES -o "$OUT/ps2netcon_gui.exe" -luser32 -lgdi32 -lws2_32
	;;
clean)
	rm -rf "$OUT"
	;;
*)
	echo "usage: $0 [unix|win32|clean]" >&2
	exit 1
	;;
esac
