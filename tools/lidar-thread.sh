#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Thread border router: the EFR32 radio running this project's OpenThread
# RCP (efr32/ in the source tree), driven by otbr-agent. Home Assistant's
# OpenThread Border Router integration talks to its REST API at
# http://<echo>:8081; ot-ctl is the command line on the device.
#
# The RCP image ships in this system. lidar-efr32 uploads it through the
# EFR32's bootloader only if the application there does not answer as an
# OpenThread RCP (nothing there, or the zigbee service's NCP).
#
# The Thread network (key, dataset, frame counters) lives in /var/lib/thread,
# bound from the same directory on the booted slot: the slot's filesystem
# is made writable underneath while its mount at /media/system stays
# read-only, so this directory is the only place that can be written.
#
# Usage: lidar-thread daemon   bring the RCP up, then run otbr-agent
#        lidar-thread status
GBL=/usr/share/lidar/efr32/lidar-ot-rcp.gbl
SLOT=/media/system
DATA=/var/lib/thread
RADIO='spinel+hdlc+uart:///dev/ttyS2?uart-baudrate=115200'
GET_VERSION='\176\201\002\002\136\200\176'	# Spinel PROP_VALUE_GET NCP_VERSION

log() { echo "lidar-thread: $*" > /dev/kmsg; }

rcp_running() { # the RCP answers "OPENTHREAD/..."
	case $(lidar-efr32 exchange "$GET_VERSION" 1) in
	*" 4f 50 45 4e 54 48 52 45 41 44 "*) return 0 ;;
	esac
	return 1
}

bring_up() {
	lidar-efr32 run || { log "cannot start the EFR32 application"; return 1; }
	rcp_running && return 0
	log "no OpenThread RCP answers, uploading ${GBL##*/}"
	if ! [ -r $GBL ] || ! lidar-efr32 flash $GBL; then
		log "upload failed"
		return 1
	fi
	rcp_running && { log "RCP running"; return 0; }
	log "the uploaded RCP does not start"
	return 1
}

keep_data() {
	mountpoint -q $DATA && return 0
	if [ ! -d $SLOT/etc ]; then
		log "booted slot not mounted: nowhere to keep the Thread network"
		return 1
	fi
	mount -o remount,rw $SLOT || return 1
	mkdir -p $SLOT$DATA && chmod 700 $SLOT$DATA
	ok=$?
	# back to read-only for this mount only: the filesystem stays writable
	mount -o remount,bind,ro $SLOT || return 1
	[ $ok -eq 0 ] &&
		mkdir -p $DATA &&
		mount --bind $SLOT$DATA $DATA &&
		mount -o remount,bind,rw $DATA
}

case $1 in
daemon)
	infra=$(lidar-feature get thread infra)
	# usb: a unix socket, which init's lidar-listen bridges to port 8081 for
	# connections arriving on usb0 only (an address alone would also be
	# reachable through wlan0)
	rest=/run/otbr-rest.sock
	[ "$(lidar-feature get thread listen)" = any ] && rest=::
	# Wi-Fi comes up in the background after boot
	for i in $(seq 1 60); do
		[ -d /sys/class/net/$infra ] && break
		sleep 1
	done
	if [ ! -d /sys/class/net/$infra ] || ! keep_data || ! bring_up; then
		[ -d /sys/class/net/$infra ] || log "no $infra"
		sleep 60	# init respawns the daemon; do not hammer the radio
		exit 1
	fi
	# Border routing: forward IPv6, and keep taking router advertisements
	# (with route information) on the infrastructure link while forwarding.
	echo 1 > /proc/sys/net/ipv6/conf/all/forwarding
	echo 2 > /proc/sys/net/ipv6/conf/$infra/accept_ra
	echo 64 > /proc/sys/net/ipv6/conf/$infra/accept_ra_rt_info_max_plen
	exec otbr-agent -I wpan0 -B "$infra" --data-path $DATA -s -d 5 \
		--rest-listen-address $rest "$RADIO" 2> /dev/kmsg
	;;
status)
	if pgrep -x otbr-agent >/dev/null; then
		echo "otbr-agent: running, Thread $(ot-ctl state | head -n1)"
	else
		echo "otbr-agent: not running"
	fi
	;;
*)
	sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
