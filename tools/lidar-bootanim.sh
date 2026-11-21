#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Light-ring boot animation. Each of the 12 LEDs owns a hue of a colour
# wheel; two comets on opposite sides reveal it as they spin, so they are
# always complementary, and the wheel drifts one step per revolution. When
# the stop file appears, a rainbow sweep fills the ring and fades out.
#
# Usage: lidar-bootanim [stop-file]   (default /run/boot-done)
STOP=${1:-/run/boot-done}
L=/sys/class/leds
N=12

# Hue of wheel slot $1 (0-11) -> R G B
hue() {
	case $1 in
	0) R=255 G=0 B=0 ;;
	1) R=255 G=96 B=0 ;;
	2) R=255 G=200 B=0 ;;
	3) R=160 G=255 B=0 ;;
	4) R=0 G=255 B=40 ;;
	5) R=0 G=255 B=160 ;;
	6) R=0 G=220 B=255 ;;
	7) R=0 G=96 B=255 ;;
	8) R=40 G=0 B=255 ;;
	9) R=160 G=0 B=255 ;;
	10) R=255 G=0 B=200 ;;
	*) R=255 G=0 B=80 ;;
	esac
}

set_led() { # n(1-12) r g b
	echo "$2" > "$L/ring$1:red/brightness"
	echo "$3" > "$L/ring$1:green/brightness"
	echo "$4" > "$L/ring$1:blue/brightness"
}

# LED $1 (0-11) in its wheel colour, divided by $2 (0 = off)
paint() {
	if [ "$2" -eq 0 ]; then
		set_led $(($1 + 1)) 0 0 0
	else
		hue $(( ($1 + drift) % N ))
		set_led $(($1 + 1)) $((R / $2)) $((G / $2)) $((B / $2))
	fi
}

[ -e "$L/ring1:red/brightness" ] || exit 0
i=1
while [ "$i" -le $N ]; do
	set_led "$i" 0 0 0
	i=$((i + 1))
done

head=0 drift=0 frames=0
# At least two revolutions, so a fast boot still shows the animation.
while [ ! -e "$STOP" ] || [ "$frames" -lt $((2 * N)) ]; do
	# Head, two tail steps, and the LED the tail just left, for both comets.
	for c in 0 6; do
		h=$(( (head + c) % N ))
		paint $h 1
		paint $(( (h + N - 1) % N )) 3
		paint $(( (h + N - 2) % N )) 10
		paint $(( (h + N - 3) % N )) 0
	done
	head=$(( (head + 1) % N ))
	[ "$head" -eq 0 ] && drift=$(( (drift + 1) % N ))
	frames=$((frames + 1))
	sleep 0.07
done

# Finale: sweep the full wheel in, hold, fade out.
i=0
while [ "$i" -lt $N ]; do
	paint $(( (head + i) % N )) 1
	i=$((i + 1))
	sleep 0.03
done
sleep 0.25
for f in 2 4 8 16 0; do
	i=0
	while [ "$i" -lt $N ]; do
		paint $i $f
		i=$((i + 1))
	done
	sleep 0.06
done
