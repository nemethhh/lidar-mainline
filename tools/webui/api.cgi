#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Web UI backend (BusyBox httpd CGI): device statistics, volume, the song
# library and the player. One query per request, JSON out; see the README.
#
# Reads are GET (stats, player, songs, services). Commands must be POST with
# Content-Type application/json, uploads POST with application/octet-stream:
# a web page elsewhere in the host's browser cannot send either without a
# CORS preflight, which this server never answers. The Host header must be
# an IP address, which keeps DNS rebinding out.
LIB=/run/lidar-player
FIFO=/run/lidar-player.fifo

printf 'Content-Type: application/json\r\nCache-Control: no-store\r\n\r\n'

fail() { printf '{"error": "%s"}\n' "$1"; exit 0; }
ok() { printf '{"ok": true}\n'; exit 0; }

# JSON string body: backslash and quote escaped, control characters dropped
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\000-\037'; }

# file -> $v (first line); $2 (default null) if unreadable or empty; no fork
rdv() {
	v=
	[ -r "$1" ] && read -r v < "$1"
	[ -n "$v" ] || v=${2-null}
}

player() { # the player's own state file, or stopped
	if [ -r /run/lidar-player.json ]; then
		cat /run/lidar-player.json
	else
		echo '{"state": "stopped", "id": null, "title": null, "elapsed": 0, "duration": 0}'
	fi
}

send() { # command -> player
	[ -p $FIFO ] || fail "player not running"
	echo "$*" > $FIFO
	ok
}

limit_bytes() {
	mb=$(lidar-feature get webui storage_mb 2>/dev/null)
	echo $(( ${mb:-256} * 1024 * 1024 ))
}

used_bytes() {
	u=$(du -sb $LIB 2>/dev/null | cut -f1)
	echo "${u:-0}"
}

valid_id() { case $1 in '' | *[!0-9-]*) return 1 ;; esac; }

stats() {
	read -r up _ < /proc/uptime
	read -r l1 l5 l15 _ < /proc/loadavg
	read -r _ u n s i w q sq _ < /proc/stat
	model=$(tr -d '\000' < /proc/device-tree/model)
	printf '{"time": %s, "uptime": %s, "model": "%s", "kernel": "%s",\n' \
		"$(date +%s)" "$up" "$(esc "$model")" "$(uname -r)"
	printf ' "load": [%s, %s, %s],\n' "$l1" "$l5" "$l15"
	f=
	for c in /sys/devices/system/cpu/cpu[0-9]*; do
		rdv $c/cpufreq/scaling_cur_freq
		f="$f${f:+, }$v"
	done
	rdv /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq; mx=$v
	rdv /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ''; gov=$v
	printf ' "cpu": {"jiffies": [%s, %s, %s, %s, %s, %s, %s], "freq_khz": [%s], "max_khz": %s, "governor": "%s"},\n' \
		"$u" "$n" "$s" "$i" "$w" "$q" "$sq" "$f" "$mx" "$(esc "$gov")"
	awk '/^MemTotal:/ { t = $2 } /^MemAvailable:/ { a = $2 }
		END { printf " \"mem\": {\"total_kb\": %d, \"available_kb\": %d},\n", t, a }' /proc/meminfo
	# board temperature sensors: "<chip> <bus-addr> <millidegrees>" per line
	t=
	for h in /sys/class/hwmon/hwmon*; do
		[ -r $h/temp1_input ] || continue
		rdv $h/name '?'; chip=$v
		rdv $h/temp1_input; mc=$v
		dev=$(readlink -f $h/device)
		t="$t$chip ${dev##*/} $mc
"
	done
	printf '%s' "$t" | awk 'BEGIN { printf " \"temps\": [" }
		NF == 3 { printf "%s{\"name\": \"%s (i2c%s 0x%s)\", \"c\": %s}", n++ ? ", " : "",
			toupper($1), substr($2, 1, index($2, "-") - 1), substr($2, index($2, "-") + 3),
			$3 == "null" ? "null" : sprintf("%.2f", $3 / 1000) }
		END { print "]," }'
	light=null
	for d in /sys/bus/iio/devices/iio:device*; do
		rdv $d/name ''
		[ "$v" = tsl2540 ] || continue
		rdv $d/in_illuminance_raw; vis=$v
		rdv $d/in_intensity_ir_raw; ir=$v
		light="{\"visible\": $vis, \"ir\": $ir}"
	done
	printf ' "light": %s,\n' "$light"
	r=
	for g in /sys/class/regulator/regulator.*; do
		rdv $g/microvolts 0
		[ "$v" != 0 ] || continue	# unreadable, or the dummy regulator
		uv=$v
		rdv $g/name ''
		r="$r${r:+, }{\"name\": \"$(esc "$v")\", \"mv\": $((uv / 1000))}"
	done
	printf ' "rails": [%s],\n' "$r"
	vol=null
	if [ -r /run/lidar-volume.state ]; then
		read -r level mute vmax < /run/lidar-volume.state
		m=false; [ "$mute" = 1 ] && m=true
		[ "${vmax:-0}" -gt 0 ] && vol=$(awk -v l="$level" -v v="$vmax" -v m=$m 'BEGIN {
			printf "{\"percent\": %d, \"muted\": %s, \"db\": %.1f}", (l * 100 + v / 2) / v, m, (l - 127) / 2 }')
	fi
	printf ' "volume": %s,\n' "$vol"
	mic=false
	ev=$(awk '/^N: Name="gpio-keys"/ { k = 1 } k && /^H: / {
		for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i; exit }' /proc/bus/input/devices)
	if [ -n "$ev" ]; then
		evtest --query /dev/input/$ev EV_SW SW_MUTE_DEVICE >/dev/null 2>&1
		[ $? = 10 ] && mic=true
	fi
	dsp=false; [ -e /run/lidar-dsp/ready ] && dsp=true
	printf ' "mic_muted": %s, "dsp": %s,\n' "$mic" "$dsp"
	# LED ring: 12 x red, green, blue, scaled to 0..255, one awk for all
	files=
	for k in 1 2 3 4 5 6 7 8 9 10 11 12; do
		for c in red green blue; do
			files="$files /sys/class/leds/ring$k:$c/brightness"
		done
	done
	rdv /sys/class/leds/ring1:red/max_brightness 255; mb=$v
	# shellcheck disable=SC2086 # the list is meant to split
	awk -v mb="$mb" 'BEGIN { if (mb <= 0) mb = 255 } FNR == 1 { b[n++] = int($1 * 255 / mb) }
		END { printf " \"ring\": ["
		      for (i = 0; i < 36; i += 3) printf "%s[%d, %d, %d]", i ? ", " : "", b[i], b[i + 1], b[i + 2]
		      print "]," }' $files 2>/dev/null
	rdv /sys/class/net/usb0/statistics/rx_bytes; rx=$v
	rdv /sys/class/net/usb0/statistics/tx_bytes; tx=$v
	printf ' "net": {"rx_bytes": %s, "tx_bytes": %s},\n' "$rx" "$tx"
	printf ' "player": %s}\n' "$(player)"
}

services() {
	sv=
	while read -r name group on _ _ desc; do
		[ -n "$name" ] || continue
		e=false; [ "$on" = on ] && e=true
		sv="$sv${sv:+, }{\"name\": \"$(esc "$name")\", \"group\": \"$(esc "$group")\", \"enabled\": $e, \"description\": \"$(esc "$desc")\"}"
	done <<-EOF
	$(lidar-feature list)
	EOF
	printf '{"services": [%s]}\n' "$sv"
}

songs() {
	s=
	for f in $LIB/*.wav; do
		[ -e "$f" ] || continue
		id=${f##*/}; id=${id%.wav}
		rdv $LIB/$id.title ''; title=$v
		rdv $LIB/$id.duration 0; d=$v
		s="$s${s:+,
 }{\"id\": \"$id\", \"title\": \"$(esc "$title")\", \"duration\": $d, \"bytes\": $(stat -c %s "$f" 2>/dev/null || echo 0)}"
	done
	printf '{"songs": [%s],\n "used_bytes": %s, "limit_bytes": %s}\n' "$s" "$(used_bytes)" "$(limit_bytes)"
}

upload() {
	len=${CONTENT_LENGTH:-0}
	case $len in '' | *[!0-9]*) fail "bad length" ;; esac
	[ "$len" -gt 44 ] || fail "empty upload"
	[ -d $LIB ] || fail "player not running"
	# leftovers of uploads whose CGI died
	for f in $LIB/.upload.*; do
		[ -e "$f" ] && ! kill -0 "${f##*-}" 2>/dev/null && rm -f "$f"
	done
	limit=$(limit_bytes)
	# 64 KB stay free for the small title and duration files
	[ $(( $(used_bytes) + len + 65536 )) -le "$limit" ] ||
		fail "library full (limit $((limit / 1024 / 1024)) MB)"
	id=$(printf '%012d-%05d' "$(date +%s)" $$)
	tmp=$LIB/.upload.$id
	trap 'rm -f "$tmp"' EXIT
	head -c "$len" > "$tmp"
	# short: the client went away, or the library's tmpfs is full
	[ "$(stat -c %s "$tmp")" = "$len" ] || fail "incomplete upload (library full?)"
	d=$(lidar-player info "$tmp" 2>/dev/null) || fail "not a 48 kHz 16-bit stereo PCM WAV"
	t=$(busybox-extras httpd -d "$title" | tr -d '\000-\037' | cut -c1-200)
	printf '%s\n' "${t:-Untitled}" > $LIB/$id.title
	echo "$d" > $LIB/$id.duration
	mv "$tmp" $LIB/$id.wav
	trap - EXIT
	printf '{"ok": true, "id": "%s"}\n' "$id"
}

# the server is reached by its address, never by a name
case ${HTTP_HOST%:*} in '' | *[!0-9.]*) fail "bad host" ;; esac

# QUERY_STRING: one command, e.g. "stats", "play=<id>", "upload&title=..."
q=${QUERY_STRING%%&*}
key=${q%%=*}
val=; [ "$key" != "$q" ] && val=${q#*=}
title=
case $QUERY_STRING in *'&title='*) title=${QUERY_STRING#*&title=}; title=${title%%&*} ;; esac

case $key in
stats | player | songs | services) ;;
upload)
	[ "$REQUEST_METHOD" = POST ] && [ "$CONTENT_TYPE" = application/octet-stream ] ||
		fail "upload needs POST with application/octet-stream"
	;;
*)
	[ "$REQUEST_METHOD" = POST ] && [ "$CONTENT_TYPE" = application/json ] ||
		fail "commands need POST with application/json"
	;;
esac

case $key in
stats) stats ;;
player) player ;;
songs) songs ;;
services) services ;;
upload) upload ;;
play) valid_id "$val" && [ -e $LIB/$val.wav ] || fail "no such song"; send play "$val" ;;
pause | resume | stop | next | prev) send "$key" ;;
seek)
	case $val in '' | *[!0-9.]* | ?????????*) fail "bad position" ;; esac
	send seek "$val"
	;;
delete)
	valid_id "$val" && [ -e $LIB/$val.wav ] || fail "no such song"
	case $(player) in *"\"id\": \"$val\""*) [ -p $FIFO ] && echo stop > $FIFO ;; esac
	rm -f $LIB/$val.wav $LIB/$val.title $LIB/$val.duration
	ok
	;;
volume)
	case $val in '' | *[!0-9]* | ????*) fail "bad volume" ;; esac
	[ "$val" -le 100 ] || fail "bad volume"
	lidar-volume set "$val" >/dev/null 2>&1 || fail "volume service not running"
	ok
	;;
mute)
	case $val in
	1) lidar-volume mute >/dev/null 2>&1 || fail "volume service not running" ;;
	0) lidar-volume unmute >/dev/null 2>&1 || fail "volume service not running" ;;
	*) fail "bad mute" ;;
	esac
	ok
	;;
*) fail "unknown request" ;;
esac
