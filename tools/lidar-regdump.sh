#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Dump the SoC state relevant to MSDC2 / MT7668 bring-up in a diffable form.
# Same script under the stock 4.4 kernel and under mainline.
dump() { # name base words
	a=$2; n=0
	while [ $n -lt $3 ]; do
		printf 'R %s %08x %s\n' "$1" $((a + n * 4)) "$(devmem $((a + n * 4)))"
		n=$((n + 1))
	done
}
echo "REGDUMP BEGIN kernel=$(uname -r)"
for d in /sys/bus/sdio/devices/*; do
	[ -e "$d" ] && echo "SDIO $(basename "$d") $(cat "$d/vendor") $(cat "$d/device")"
done
dmesg | grep -iE 'mmc1|msdc|sdio|11170000' | tail -20 | sed 's/^/K /'
dump msdc2 0x11170000 128
dump topck 0x10000000 64
dump infra 0x10001000 64
dump apmix 0x10018000 64
dump pio 0x10005000 1024
echo "REGDUMP END"
