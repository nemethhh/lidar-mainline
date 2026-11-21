#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# USB speakerphone: add a USB Audio Class 2 function to the running gadget,
# so the USB host sees the Echo as a sound card, and bridge it to the
# speaker and the microphones.
#
#   host playback (stereo, 48 kHz) -> Echo speaker
#   mics 1-6 mixed to mono         -> host capture
#
# Usage: lidar-usbaudio start|stop|status
#
# If the host had configured the gadget before and does not configure it
# again within 15 s after the audio function is added, the function is
# removed and the previous gadget is restored, so the USB shell and network
# come back. Off by default; opt in with: lidar-feature enable usbaudio
G=/sys/kernel/config/usb_gadget/lidar
F=$G/functions/uac2.usb0
CARD=UAC2Gadget
RUN=/run/lidar-usbaudio
# Unbinding the gadget hangs up the USB serial console. Run from that shell,
# SIGHUP would kill this script between unbind and bind and leave USB dead.
trap '' HUP

udc_state() { cat "/sys/class/udc/$(cat $G/UDC 2>/dev/null)/state" 2>/dev/null; }

# Functions can only be linked into the configuration while it is unbound.
unbind() {
	echo "" > $G/UDC 2>/dev/null
	sleep 0.5
}

bind() {
	ls /sys/class/udc | head -n1 > $G/UDC
}

# The gadget reports the rate the host selected, or 0 while it is not
# streaming. The speaker pipe stays up while the function exists; the mic
# pipe (six-mic mix) only runs while the host records.
# Plain blocking arecord|aplay pipes sleep in the kernel instead of polling.
rate() {
	amixer -c $CARD cget iface=PCM,name="$1 Rate" 2>/dev/null |
		sed -n 's/^  : values=//p'
}

pipe_alive() { [ -f $RUN.$1 ] && kill -0 "$(cat $RUN.$1)" 2>/dev/null; }

pipe_start() { # name command
	setsid sh -c "$2" </dev/null >/dev/null 2>&1 &
	echo $! > $RUN.$1
}

pipe_stop() { # kill the whole pipeline: its process group
	[ -f $RUN.$1 ] && kill -TERM -"$(cat $RUN.$1)" 2>/dev/null
	rm -f $RUN.$1
}

# Owner's mic level (lidar-feature settings usbaudio), on top of init's base
# audio setup. An unreadable setting leaves the base level alone. Speaker
# volume belongs to the volume service (lidar-volume).
levels() {
	v=$(lidar-feature get usbaudio mic_gain) && [ -n "$v" ] &&
		for n in 1 2 3 4; do
			amixer -q -c amazonlidar cset name="ADC$n PGA Capture Volume" "$v,$v"
		done
}

# Host volume for the speaker direction. With the volume service the gadget
# offers a hardware volume whose steps are the codec's 0.5 dB steps up to
# the safety cap, so the host's own volume curve applies unchanged and
# lidar-volume copies the value to the codec. Without it the gadget offers
# no volume and the host scales the audio itself. The mic direction never
# has one: the host applies its own gain.
host_volume() {
	echo 0 > $F/p_volume_present
	echo 0 > $F/p_mute_present
	if lidar-feature enabled volume; then
		a=$(lidar-feature get volume max_attenuation)
		steps=$((127 - 2 * ${a:-20}))
		echo 0 > $F/c_volume_max
		echo $((-steps * 128)) > $F/c_volume_min	# 1/256 dB units
		echo 128 > $F/c_volume_res			# 0.5 dB
	else
		echo 0 > $F/c_volume_present
		echo 0 > $F/c_mute_present
	fi
}

pipe_while_streaming() { # name direction command
	r=$(rate "$2")
	if [ -n "$r" ] && [ "$r" != 0 ]; then
		pipe_alive "$1" || pipe_start "$1" "$3"
	else
		pipe_stop "$1"
	fi
}

supervise() {
	# The bridge's aplay stays open and only the reader restarts: reading
	# the gadget fails with EIO whenever the host is not streaming. The
	# codec output itself is held open by lidar-volume's silent stream
	# (volume keepalive), so host streams start at once and without clicks.
	# Without that keep-alive the bridge only runs while the host plays, so
	# the speaker output can close in between.
	persistent=
	lidar-feature enabled volume &&
		[ "$(lidar-feature get volume keepalive)" != off ] && persistent=1
	spk="while [ -e /proc/asound/$CARD ]; do
	       arecord -q -D hw:$CARD,0 -t raw -f S16_LE -c 2 -r 48000 2>/dev/null ||
	         sleep 0.1
	     done | aplay -q -D lidar_speaker -t raw -f S16_LE -c 2 -r 48000 2>/dev/null"
	# Echo -> host: by default through lidar-aec, which removes the echo of
	# whatever the speaker plays (the host's own audio included) and steady
	# background noise. It holds the mic and reference devices only while
	# the host records.
	src="arecord -q -D lidar_mics -t raw -f S16_LE -c 1 -r 48000"
	[ "$(lidar-feature get usbaudio aec)" = off ] || src=lidar-aec
	mic="$src | aplay -q -D hw:$CARD,0 -t raw -f S16_LE -c 1 -r 48000"
	while [ -d $F ]; do
		if [ -n "$persistent" ]; then
			pipe_alive spk || pipe_start spk "$spk"	# host -> Echo
		else
			pipe_while_streaming spk Capture "$spk"
		fi
		pipe_while_streaming mic Playback "$mic"	# Echo -> host
		sleep 1
	done
	pipe_stop spk
	pipe_stop mic
}

bridge_start() {
	for i in $(seq 1 50); do [ -e /proc/asound/$CARD ] && break; sleep 0.2; done
	[ -e /proc/asound/$CARD ] || { echo "no $CARD sound card" >&2; return 1; }
	setsid "$0" supervise </dev/null >/dev/null 2>&1 &
	echo $! > $RUN.sup
}

bridge_stop() {
	[ -f $RUN.sup ] && kill "$(cat $RUN.sup)" 2>/dev/null
	rm -f $RUN.sup
	pipe_stop spk
	pipe_stop keep
	pipe_stop mic
}

function_remove() {
	bridge_stop
	unbind
	rm -f $G/configs/c.1/uac2.usb0
	rmdir $F 2>/dev/null
	bind
}

case $1 in
start)
	[ -d $G ] || { echo "USB gadget not set up" >&2; exit 1; }
	[ -d $F ] && { echo "already started"; exit 0; }
	before=$(udc_state)
	mkdir $F || exit 1
	echo 3 > $F/c_chmask	# host -> Echo: stereo
	echo 48000 > $F/c_srate
	echo 2 > $F/c_ssize
	echo 1 > $F/p_chmask	# Echo -> host: mono microphone
	echo 48000 > $F/p_srate
	echo 2 > $F/p_ssize
	echo "Echo Plus speakerphone" > $F/function_name
	host_volume
	unbind
	if ! ln -s $F $G/configs/c.1/; then
		rmdir $F
		bind
		exit 1
	fi
	bind || { echo "bind failed, rolling back" >&2; function_remove; exit 1; }
	if [ "$before" = configured ]; then
		for i in $(seq 1 30); do [ "$(udc_state)" = configured ] && break; sleep 0.5; done
		if [ "$(udc_state)" != configured ]; then
			echo "host did not accept the audio function, rolling back" >&2
			function_remove
			exit 1
		fi
	fi
	levels
	# Not "a && b || c": echo fails on the hung-up USB console and must not
	# undo a successful start.
	if bridge_start; then
		# A new gadget control starts at its maximum: let the volume
		# daemon push its level to it.
		[ -p /run/lidar-volume.fifo ] && lidar-volume sync
		echo "USB speakerphone started"
	else
		function_remove
		exit 1
	fi
	;;
stop)
	[ -d $F ] || { echo "not started"; exit 0; }
	function_remove
	echo "USB speakerphone stopped"
	;;
status)
	if [ -d $F ]; then
		echo "function: present, gadget: $(udc_state)"
		pipe_alive sup && echo "supervisor: running" || echo "supervisor: not running"
		pipe_alive spk && echo "speaker stream: active" || echo "speaker stream: idle"
		pipe_alive mic && echo "mic stream: active" || echo "mic stream: idle"
	else
		echo "function: absent"
	fi
	;;
supervise)
	supervise
	;;
*)
	sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
