#!/bin/sh
# Firmware links for the MT7668 WiFi/BT combo (run at boot, idempotent).
#
# Shipped in the image (redistributable): the ROM patch from linux-firmware.
# The WLAN RAM firmware, the EEPROM, the transmit-power limits and wifi.cfg
# exist only on the device (/system/vendor/firmware) and are staged by the
# M0 harvest - copy them next to this script's links; until then
# wlan_mt76x8_sdio fails its firmware request, which is expected.
FW=${1:-/usr/lib/firmware}

# mainline btmtksdio requests mediatek/mt7668pr2h.bin directly; Amazon's
# WLAN driver asks for the same patch under its own E2 name
ln -snf mediatek/mt7668pr2h.bin "$FW/mt7668_patch_e2_hdr.bin"

# M0 harvest set (absent until harvested from the rooted unit):
for f in WIFI_RAM_CODE_MT7668.bin WIFI_RAM_CODE2_SDIO_MT7668.bin \
	EEPROM_MT7668.bin TxPwrLimit_MT76x8.dat wifi.cfg; do
	[ -e "$FW/$f" ] || echo "donut-firmware: $FW/$f missing (M0 harvest)" >&2
done
