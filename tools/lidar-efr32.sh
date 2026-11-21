#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The EFR32 radio's Gecko bootloader, seen from the SoC (ttyS2, 115200 8N1,
# no flow control). The bootloader starts the application only while
# EFR32_SWCLK is high; low keeps it in its menu ("BL >"), which is where the
# board holds it at power-on. Only the application area is ever written,
# through the bootloader's own XMODEM upload: never the bootloader itself or
# the chip's factory data.
#
# Usage: lidar-efr32 run           start the application if the bootloader
#                                  waits in its menu; keep SWCLK high
#        lidar-efr32 flash <gbl>   upload an application image and start it
#        lidar-efr32 exchange <printf format> <seconds>
#                                  send bytes, print the reply as hex
TTY=/dev/ttyS2
RUN=/run/lidar-efr32

log() { echo "lidar-efr32: $*" > /dev/kmsg; }

# Sends $1 (printf format) on fd 3 (the open tty) and prints the reply of
# the next $2 seconds as hex bytes separated by spaces.
exchange() {
	timeout "$2" cat <&3 > $RUN.rx &
	r=$!
	printf "$1" >&3
	wait $r
	echo " $(od -An -tx1 -v $RUN.rx | tr -s ' \n' '  ') "
}

in_bootloader() {
	case $(exchange '\r' 1) in *" 42 4c 20 3e "*) return 0 ;; esac	# "BL >"
	return 1
}

swclk() { # 0|1: hold the line (gpioset -z keeps running to hold it)
	pkill -f 'gpioset -z EFR32_SWCLK'
	while pgrep -f 'gpioset -z EFR32_SWCLK' >/dev/null; do sleep 0.1; done
	gpioset -z EFR32_SWCLK="$1"
}

reset_chip() {
	gpioset -t 200ms,0 EFR32_RESET_N=0
	sleep 1
}

open_tty() {
	stty -F $TTY 115200 raw -echo -crtscts -ixon -ixoff || return 1
	exec 3<> $TTY
}

case $1 in
exchange)
	[ $# -eq 3 ] || { echo "usage: lidar-efr32 exchange <printf format> <seconds>" >&2; exit 2; }
	open_tty || exit 1
	exchange "$2" "$3"
	;;
run)
	open_tty || exit 1
	pgrep -f 'gpioset -z EFR32_SWCLK=1' >/dev/null || swclk 1 || exit 1
	if in_bootloader; then
		printf 2 >&3	# run: resets into the application
		sleep 1
	fi
	;;
flash)
	[ -r "$2" ] || { echo "usage: lidar-efr32 flash <file.gbl>" >&2; exit 2; }
	open_tty || exit 1
	swclk 0 || exit 1
	reset_chip
	if ! in_bootloader; then
		log "the bootloader does not answer"
		swclk 1
		exit 1
	fi
	log "uploading ${2##*/}"
	printf 1 >&3	# upload gbl: the bootloader now waits for XMODEM
	if ! lidar-xmodem $TTY "$2"; then
		log "upload of ${2##*/} failed"
		swclk 1
		exit 1
	fi
	swclk 1 || exit 1
	reset_chip
	log "${2##*/} uploaded and started"
	;;
*)
	sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
