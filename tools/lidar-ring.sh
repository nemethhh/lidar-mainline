#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Owner of the LED ring after boot. Base state as on stock: solid red while
# the microphones are muted by the privacy button, off otherwise. Other
# programs send short overlays through /run/lidar-ring.fifo:
#   volume <level> <muted 0|1> <max>   arc of white LEDs (dimmed to 15% while
#                                      the speaker is muted), shown for
#                                      HOLD seconds after the last change
# A change of the privacy state ends any overlay at once.
# Brightness follows the ambient light sensor, as on stock: full from a lit
# room down to "ring min_brightness" percent in the dark (setting "ring
# auto_brightness off" keeps it at full).
# Writers should open the FIFO read-write ("exec 5<>/run/lidar-ring.fifo") so
# a write never blocks.
#
# Usage: lidar-ring daemon
FIFO=/run/lidar-ring.fifo
L=/sys/class/leds
N=12
HOLD=2
WHITE=96
RED=160
# Sensor counts at its most sensitive setting: covered reads 0-20, a dimly
# lit room about 550, a flashlight about 9000. Brightness is interpolated on
# a log scale between ALS_DARK (minimum) and ALS_BRIGHT (full).
ALS_DARK=30
ALS_BRIGHT=8000

set_led() { # n r g b
	echo "$2" > "$L/ring$1:red/brightness"
	echo "$3" > "$L/ring$1:green/brightness"
	echo "$4" > "$L/ring$1:blue/brightness"
}

fill() { # r g b
	i=1
	while [ $i -le $N ]; do set_led $i "$1" "$2" "$3"; i=$((i + 1)); done
}

# Ambient light sensor (TSL2540): most sensitive gain and longest
# integration, so a dark room still reads hundreds of counts.
als_setup() {
	als=
	[ "$(lidar-feature get ring auto_brightness 2>/dev/null)" = off ] && return
	for d in /sys/bus/iio/devices/iio:device*; do
		[ "$(cat "$d/name" 2>/dev/null)" = tsl2540 ] && als=$d
	done
	[ -n "$als" ] || return
	tr ' ' '\n' < "$als/in_illuminance_scale_available" | sort -g | head -n1 \
		> "$als/in_illuminance_scale"
	tr -d '[]' < "$als/integration_time_available" | awk '{ print $NF }' \
		> "$als/integration_time"
}

level() { # -> P, the brightness in percent
	P=100
	[ -n "$als" ] || return
	P=$(awk -v v="$(cat "$als/in_illuminance_raw" 2>/dev/null)" -v min="$dim_min" \
		-v lo=$ALS_DARK -v hi=$ALS_BRIGHT 'BEGIN {
		if (v == "" || v >= hi) { print 100; exit }
		if (v <= lo) { print min; exit }
		printf "%d\n", min + (100 - min) * (log(v) - log(lo)) / (log(hi) - log(lo))
	}')
}

scaled() { # value -> value * P / 100, at least 1 unless 0
	s=$(( $1 * P / 100 ))
	[ "$1" -gt 0 ] && [ $s -lt 1 ] && s=1
	echo $s
}

base() {
	level
	p_drawn=$P
	if [ "$mic_muted" = 1 ]; then fill $(scaled $RED) 0 0; else fill 0 0 0; fi
}

# The arc covers the level in brightness units: full LEDs, then one partly
# lit LED, so every volume step changes the ring (12 LEDs, 21 button steps).
# While the speaker is muted the same arc shows at 15%: muted, and the level
# it comes back at.
volume() { # level muted max
	[ "$3" -gt 0 ] 2>/dev/null && [ "$1" -ge 0 ] 2>/dev/null || return
	level
	w=$(scaled $WHITE)
	t=$(( ($1 * N * w + $3 - 1) / $3 ))
	[ "$2" = 1 ] && [ $t = 0 ] && t=1	# muted at level 0: still a mark
	i=1
	while [ $i -le $N ]; do
		v=$t; [ $v -gt $w ] && v=$w
		if [ "$2" = 1 ] && [ $v -gt 0 ]; then
			v=$((v * 15 / 100)); [ $v -lt 1 ] && v=1
		fi
		set_led $i $v $v $v
		t=$((t - w)); [ $t -lt 0 ] && t=0
		i=$((i + 1))
	done
}

# Privacy latch (SW_MUTE_DEVICE on gpio-keys) into the FIFO
mic_watch() {
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	[ -n "$ev" ] || return
	evtest --query "/dev/input/$ev" EV_SW SW_MUTE_DEVICE
	[ $? = 10 ] && echo "mic 1" >&3 || echo "mic 0" >&3
	evtest "/dev/input/$ev" 2>/dev/null | while read -r line; do
		case $line in
		*SW_MUTE_DEVICE*'value 1'*) echo "mic 1" >&3 ;;
		*SW_MUTE_DEVICE*'value 0'*) echo "mic 0" >&3 ;;
		esac
	done
}

daemon() {
	[ -e "$L/ring1:red/brightness" ] || exit 0
	[ -p $FIFO ] || mkfifo $FIFO || exit 1
	exec 3<> $FIFO
	# The boot animation owns the ring until it has finished its fade-out.
	while [ ! -e /run/boot-done ] || pidof lidar-bootanim >/dev/null; do
		sleep 0.2
	done
	# Overlays sent in the meantime are stale: show none of them.
	while read -t 0.05 _ <&3; do :; done
	mic_muted=0
	overlay=
	p_drawn=100
	dim_min=$(lidar-feature get ring min_brightness 2>/dev/null)
	dim_min=${dim_min:-15}
	als_setup
	mic_watch &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	base
	while :; do
		if read -t $HOLD cmd a b c <&3; then
			case $cmd in
			mic) mic_muted=$a; overlay=; base ;;	# privacy state wins at once
			volume) volume "$a" "$b" "$c"; overlay=1 ;;
			esac
		elif [ -n "$overlay" ]; then
			overlay=
			base
		elif [ "$mic_muted" = 1 ]; then
			# follow the room light in steps that do not flicker; the end
			# values are clamped, so reaching them cannot flicker either
			level
			d=$((P - p_drawn)); [ $d -lt 0 ] && d=$((-d))
			if [ $d -ge 10 ] || { [ $d -gt 0 ] &&
			   { [ $P = 100 ] || [ $P = "$dim_min" ]; }; }; then
				base
			fi
		fi
	done
}

case $1 in
daemon) daemon ;;
*)
	sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
