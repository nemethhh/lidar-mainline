#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Zigbee coordinator: the EFR32 radio running Silicon Labs' EmberZNet NCP,
# reachable over the network for Home Assistant (ZHA) as an EZSP coordinator
# at socket://<echo>:6638.
#
# The EFR32 talks to the SoC on ttyS2; lidar-efr32 handles its Gecko
# bootloader. The application image is the owner's own copy of Fire OS's
# EmberZNet NCP (xncp-fw.gbl on system_a); it is uploaded through the
# bootloader only if the application in the EFR32 does not answer as an NCP
# (nothing there, or the thread service's RCP). After a reboot without a
# power cut the NCP simply keeps running.
#
# Usage: lidar-zigbee daemon   bring the NCP up, then bridge it to TCP
#        lidar-zigbee status
TTY=/dev/ttyS2
PORT=6638
RUN=/run/lidar-zigbee
GBL=system/vendor/firmware/silabs/xncp-fw.gbl
ASH_RST='\032\300\070\274\176'	# ASH cancel + RST frame; the NCP answers RSTACK

log() { echo "lidar-zigbee: $*" > /dev/kmsg; }

app_running() {
	case $(lidar-efr32 exchange "$ASH_RST" 2) in *" c1 02 "*) return 0 ;; esac	# RSTACK
	return 1
}

upload() {
	dev=/dev/disk/by-partlabel/system_a
	mkdir -p $RUN.fireos
	mount -o ro -t ext4 $dev $RUN.fireos || return 1
	ok=1
	[ -r "$RUN.fireos/$GBL" ] && lidar-efr32 flash "$RUN.fireos/$GBL" && ok=0
	umount $RUN.fireos
	return $ok
}

bring_up() {
	lidar-efr32 run || { log "cannot start the EFR32 application"; return 1; }
	app_running && return 0
	log "no NCP answers, uploading the NCP image from system_a"
	if ! upload; then
		log "upload failed"
		return 1
	fi
	app_running && { log "NCP running"; return 0; }
	log "the uploaded NCP does not start"
	return 1
}

case $1 in
daemon)
	if ! bring_up; then
		sleep 60	# init respawns the daemon; do not hammer the radio
		exit 1
	fi
	# usb: the socket only takes connections that arrive on usb0
	# (SO_BINDTODEVICE), whatever address they are sent to.
	bind=,so-bindtodevice=usb0
	if [ "$(lidar-feature get zigbee listen)" = any ]; then
		bind=
	elif [ ! -d /sys/class/net/usb0 ]; then
		log "no usb0 to listen on"
		sleep 60
		exit 1
	fi
	# One client at a time: the NCP is one serial line. When the client
	# leaves, socat exits and init starts the daemon again.
	exec socat TCP-LISTEN:$PORT,reuseaddr$bind $TTY,raw,echo=0,b115200
	;;
status)
	if pgrep -f "socat TCP-LISTEN:$PORT" >/dev/null; then
		echo "bridge: listening on port $PORT"
	else
		echo "bridge: not running"
	fi
	;;
*)
	sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
