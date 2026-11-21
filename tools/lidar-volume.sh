#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# System volume for the Echo speaker: one level and a mute flag, applied to
# the codec's PCM Playback Volume (0.5 dB steps). Every source of volume
# changes feeds the same state:
#   - the USB host's volume slider and mute (UAC2 gadget, lidar-usbaudio),
#     which also hears back about changes made here;
#   - the volume buttons (hold to repeat; a short tone at the new level);
#   - this command.
# Other sources (e.g. Bluetooth absolute volume) can send the same commands.
#
# The level runs from 0 (codec -63.5 dB, practically silent) to the safety
# cap set by "lidar-feature set volume max_attenuation <dB>".
#
# With the speaker tuning (dsp feature) the volume is applied by lidar-dsp,
# before its EQ and compressor as on stock: lidar-volume writes the level to
# /run/lidar-dsp/volume and, once lidar-dsp reports that its output runs,
# parks the codec at the safety cap. Until then the codec keeps the level
# too, so a missing DSP can only make the speaker quieter, never louder.
#
# The speaker output is held open by a silent stream (volume keepalive), as
# on stock: every start and stop of the codec stream clicks, either through
# the DAC clocks stopping or through the amplifier's mute pin.
#
# Usage: lidar-volume daemon
#        lidar-volume get
#        lidar-volume set <percent> | up | down | mute | unmute | toggle | sync
CODEC=amazonlidar
GADGET=UAC2Gadget
RUN=/run/lidar-volume
FIFO=$RUN.fifo
RING_FIFO=/run/lidar-ring.fifo	# lidar-ring draws the volume arc
STEP=6		# volume buttons: 3 dB per press
REPEAT_DELAY=0.4	# held button: first repeat after this many seconds,
REPEAT_EVERY=0.15	# then one step every this many seconds
TONE=/usr/share/lidar/volume-tone.wav
DSP=/run/lidar-dsp

a=$(lidar-feature get volume max_attenuation 2>/dev/null)
VMAX=$((127 - 2 * ${a:-20}))

ctl_get() { # card name -> first value
	amixer -c "$1" cget name="$2" 2>/dev/null | sed -n 's/^  : values=\([^,]*\).*/\1/p'
}

# Codec output, and the gadget's copy so the host slider follows changes
# made here. Equal values are not written again: that would only echo back
# as another host event. "show" puts the volume on the LED ring.
apply() {
	c=$level
	[ "$mute" = 1 ] && c=0
	if [ -e $DSP/active ]; then
		awk -v l="$level" -v m="$VMAX" -v u="$mute" \
			'BEGIN { printf "%.1f %.1f %d\n", (l - 127) / 2, (l - m) / 2, u }' > $DSP/volume.new &&
			mv $DSP/volume.new $DSP/volume
		# a lidar-dsp killed without cleanup leaves ready behind
		[ -e $DSP/ready ] && pidof lidar-dsp >/dev/null && c=$VMAX
	fi
	amixer -q -c $CODEC cset name='PCM Playback Volume' "$c,$c"
	if [ -e /proc/asound/$GADGET ]; then
		[ "$(ctl_get $GADGET 'PCM Capture Volume')" = "$level" ] ||
			amixer -q -c $GADGET cset name='PCM Capture Volume' "$level"
		s=on; [ "$mute" = 1 ] && s=off
		[ "$(ctl_get $GADGET 'PCM Capture Switch')" = "$s" ] ||
			amixer -q -c $GADGET cset name='PCM Capture Switch' "$s"
	fi
	echo "$level $mute $VMAX" > $RUN.state
	[ "$1" = show ] && show
}

# The ring belongs to lidar-ring (the ring service); its FIFO is opened
# read-write once it exists, so a write never blocks.
show() {
	[ "$ring_on" = on ] || return
	if [ -z "$ring_open" ]; then
		[ -p $RING_FIFO ] || return
		exec 4<> $RING_FIFO
		ring_open=1
	fi
	echo "volume $level $mute $VMAX" >&4
}

clamp() { # value -> 0..VMAX
	if [ "$1" -lt 0 ]; then echo 0
	elif [ "$1" -gt $VMAX ]; then echo $VMAX
	else echo "$1"; fi
}

# The host changed its slider or mute: take its values. Right after the
# gadget appears (or a sync) the host is only reporting its stored setting,
# not a change by the user, so the ring stays dark until quiet_until.
from_host() {
	v=$(ctl_get $GADGET 'PCM Capture Volume')
	s=$(ctl_get $GADGET 'PCM Capture Switch')
	[ -n "$v" ] && [ -n "$s" ] || return
	old="$level $mute"
	level=$(clamp "$v")
	mute=0; [ "$s" = off ] && mute=1
	if [ "$old" = "$level $mute" ] ||
	   [ "$(cut -d. -f1 /proc/uptime)" -lt "$quiet_until" ]; then
		apply
	else
		apply show
	fi
}

# Volume buttons: a press steps once, holding repeats until release. The
# feedback tone sounds on the press and, after a hold, once at the final
# level, not on every repeat.
buttons() {
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	[ -n "$ev" ] || return
	evtest "/dev/input/$ev" 2>/dev/null | {
		rp=
		held=
		stop() {
			[ -n "$rp" ] && kill "$rp" 2>/dev/null && wait "$rp" 2>/dev/null
			rp=
			held=
		}
		hold() { # direction
			stop
			(sleep $REPEAT_DELAY
			 while :; do echo "$1" >&3; sleep $REPEAT_EVERY; done) &
			rp=$!
			held=$1
		}
		while read -r line; do
			case $line in
			*KEY_VOLUMEUP*'value 1'*) echo "up tone" >&3; hold up ;;
			*KEY_VOLUMEDOWN*'value 1'*) echo "down tone" >&3; hold down ;;
			*KEY_VOLUMEUP*'value 0'*) [ "$held" = up ] && stop && echo "tone" >&3 ;;
			*KEY_VOLUMEDOWN*'value 0'*) [ "$held" = down ] && stop && echo "tone" >&3 ;;
			esac
		done
		stop
	}
}

# Short feedback tone at the current volume. A tone still playing is not cut
# off (that clicks); the new one is skipped. If the codec output is closed
# (volume keepalive off), 600 ms of silence go first: the amplifier only
# unmutes 500 ms after the DAC starts, and would otherwise chop the tone.
tone() {
	[ "$sound_on" = on ] && [ "$mute" = 0 ] && [ -r $TONE ] || return
	[ -n "$tp" ] && kill -0 "$tp" 2>/dev/null && return
	if grep -qs closed /proc/asound/$CODEC/pcm0p/sub0/status; then
		{ head -c 115200 /dev/zero; tail -c +45 $TONE; } |
			aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 2>/dev/null &
	else
		aplay -q -D lidar_speaker $TONE 2>/dev/null &
	fi
	tp=$!
}

keepalive() { # silence into dmix holds the codec output open
	while :; do
		aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 /dev/zero 2>/dev/null
		sleep 1
	done
}

host_watch() { # the gadget's card comes and goes with lidar-usbaudio
	while :; do
		if [ -e /proc/asound/$GADGET ]; then
			echo sync >&3	# start from our level, not the gadget default
			alsactl monitor hw:$GADGET 2>/dev/null | while read -r line; do
				case $line in *'PCM Capture'*) echo host >&3 ;; esac
			done
		fi
		sleep 1
	done
}

# Producers write to fd 3, inherited from the daemon: if a daemon dies, its
# leftover producers write into its own FIFO handle, never into a new
# daemon's. On TERM the whole process group goes.
daemon() {
	[ -p $FIFO ] || mkfifo $FIFO || exit 1
	exec 3<> $FIFO
	ring_on=$(lidar-feature get volume ring 2>/dev/null)
	ring_open=
	sound_on=$(lidar-feature get volume sound 2>/dev/null)
	tp=
	last_tone=
	quiet_until=0
	p=$(lidar-feature get volume initial 2>/dev/null)
	level=$(clamp $(( (${p:-30} * VMAX + 50) / 100 )))
	mute=0
	apply
	buttons &
	host_watch &
	[ "$(lidar-feature get volume keepalive 2>/dev/null)" = off ] || keepalive &
	trap 'trap "" TERM; kill 0; exit' INT TERM
	while read -r cmd arg <&3; do
		case $cmd in
		up) level=$(clamp $((level + STEP))); mute=0; apply show ;;
		down) level=$(clamp $((level - STEP))); apply show ;;
		set) level=$(clamp $(( (arg * VMAX + 50) / 100 ))); apply show ;;
		mute) mute=1; apply show ;;
		unmute) mute=0; apply show ;;
		toggle) mute=$((1 - mute)); apply show ;;
		host) from_host ;;
		sync) quiet_until=$(( $(cut -d. -f1 /proc/uptime) + 5 )); apply ;;
		refresh) apply ;;	# lidar-dsp started or stopped
		tone)	# release: only if a hold moved the level since the press
			[ "$level" != "$last_tone" ] && tone && last_tone=$level ;;
		esac
		# a press: tone right away at the new level
		if [ "$arg" = tone ]; then tone; last_tone=$level; fi
	done
}

send() {
	[ -p $FIFO ] || { echo "lidar-volume daemon is not running (lidar-feature enable volume)" >&2; exit 1; }
	echo "$*" > $FIFO
}

case $1 in
daemon) daemon ;;
get)
	[ -f $RUN.state ] || { echo "lidar-volume daemon is not running" >&2; exit 1; }
	read -r level mute vmax < $RUN.state
	db=$(awk -v l="$level" 'BEGIN { printf "%.1f", (l - 127) / 2 }')
	echo "volume $(( (level * 100 + vmax / 2) / vmax ))% (codec $level of $vmax, $db dB)$([ "$mute" = 1 ] && echo ", muted")"
	;;
set)
	case $2 in '' | *[!0-9]* | 0?*) echo "usage: lidar-volume set <0-100>" >&2; exit 2 ;; esac
	[ "$2" -le 100 ] || { echo "usage: lidar-volume set <0-100>" >&2; exit 2; }
	send set "$2"
	;;
up | down | mute | unmute | toggle | sync) send "$1" ;;
*)
	sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
