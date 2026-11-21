# MT7668 WLAN driver for the Amazon Echo Plus 2nd gen (lidar)

Out-of-tree build of MediaTek's "gen4" WLAN driver for the MT7668 SDIO combo
chip, taken from Amazon's GPL source drop for this device
(`vendor/amazon/wlan/mediatek/driver/mt76x8`) and ported from Linux 4.4 to
mainline Linux v7.3. Mainline has no driver for MT7668 Wi-Fi; Bluetooth on the
same chip is handled by the in-tree `btmtksdio`.

The source is dual licensed (GPL-2.0 or BSD) by MediaTek, see the file headers.

**Status: works on hardware** (Linux v7.3): scans on 2.4 and 5 GHz and
connects (tested on 5 GHz: 866 Mbit/s link, about 150 Mbit/s HTTPS
download), next to Bluetooth on the same chip.

## Build

The module is built against a configured and built kernel tree (it needs
`Module.symvers`, and `CONFIG_CFG80211` and `CONFIG_MMC` enabled).

Inside the `lidar-build` container, after the kernel was built with
`build.sh linux lidar`:

    podman run --rm -v lidar-src:/src -v "<work dir>:/work" lidar-build \
        bash /work/lidar-wifi/build.sh

which is

    make -C /src/linux O=/src/build/lidar M=/work/lidar-wifi \
         MO=/src/build/lidar-wifi-mod modules

`KSRC`, `KOBJ` and `MOBJ` override the three paths. The result is
`$MOBJ/wlan_mt76x8_sdio.ko`. Board options are in `config/lidar.config`.

## Install

    make -C $KSRC O=$KOBJ M=$PWD MO=$MOBJ INSTALL_MOD_PATH=<rootfs> modules_install
    depmod -b <rootfs> <kernel release>

The module autoloads through its SDIO alias (`sdio:c*v037Ad7608*`). Add

    # /etc/modprobe.d/mt7668.conf
    softdep btmtksdio pre: wlan_mt76x8_sdio

so that the WLAN function is probed (and the shared ROM patch downloaded)
before Bluetooth is set up; see "Bluetooth coexistence" below.

The interfaces are `wlan0` (station), `p2p0` (P2P) and `ap0` (soft AP;
`CFG_DRIVER_INITIAL_RUNNING_MODE=3`), all cfg80211 only. The kernel has no
wireless extensions enabled, so the
`iwpriv` handlers are unreachable; the Android style driver commands are still
available through `SIOCDEVPRIVATE + 1`. WPA3-SAE is not supported by this
build (`CFG_SUPPORT_SAE=0`).

## Firmware and calibration files

All files are requested with `request_firmware()` using bare names, i.e. they
are looked up directly in `/lib/firmware/`. None of them is part of this
repository and none may be redistributed: copy them from the FireOS system
image of your own device (they are in `/system/vendor/firmware`).

| File | Needed | Purpose |
| --- | --- | --- |
| `mt7668_patch_e2_hdr.bin` | yes | ROM patch. The name is `mt7668_patch_e<ROM version + 1>_hdr.bin`; an E1 chip asks for `..._e1_...`. Must exist even when Bluetooth already downloaded the patch. Use linux-firmware's `mediatek/mt7668pr2h.bin` under this name (below), not Amazon's. |
| `WIFI_RAM_CODE_MT7668.bin` | yes | N9 Wi-Fi firmware. Fallback names tried in order: `WIFI_RAM_CODE_MT7668`, `WIFI_RAM_CODE_MT7668_E<eco>.bin`, `WIFI_RAM_CODE_MT7668_E<eco>`. |
| `WIFI_RAM_CODE2_SDIO_MT7668.bin` | yes | CR4 firmware. Fallbacks: same four patterns, then the same with the prefix `WIFI_RAM_CODE2_MT7668`. |
| `wifi.cfg` | no | Text key/value configuration (at most 2048 bytes). Sets e.g. `EfuseBufferModeCal`, `MacOverride`/`MacAddr`, `Country`, `Wow`. |
| `EEPROM_MT7668.bin` | only if `wifi.cfg` has `EfuseBufferModeCal 1` | Per-design RF calibration ("buffer mode"), at most 1200 bytes. Without it the calibration in the chip eFuse is used. |
| `TxPwrLimit_MT76x8.dat` | recommended | Per-country TX power limits. Without it the firmware defaults apply. |

Bluetooth (`btmtksdio`, in-tree) needs `mediatek/mt7668pr2h.bin`, the same
ROM patch under the name used by linux-firmware, and both functions must
agree on one patch. Amazon's own (a 2025 build) crashes the Bluetooth
firmware under mainline `btmtksdio` at its first LE or vendor command, so the
test image installs linux-firmware's `mt7668pr2h.bin` (redistributable) and
links `mt7668_patch_e2_hdr.bin` to it. This WLAN driver runs on it unchanged.

`/data/nvram/APCFG/APRDEB/WIFI` (MediaTek phone NVRAM) is probed as well and is
simply skipped when absent.

## MAC address

FireOS passes the MAC address through Amazon's IDME (`/proc/idme/mac_addr`),
which does not exist on mainline. The driver now takes, in this order:

1. `local-mac-address` / `mac-address` / an nvmem cell on the devicetree node
   of the WLAN SDIO function (`wifi@1` below); the bootloader can fill this
   in from IDME;
2. `MacAddr` from `wifi.cfg` if `MacOverride 1` is set; otherwise the MediaTek
   NVRAM file, or the address inside `EEPROM_MT7668.bin` in buffer mode;
3. the address programmed into the chip eFuse, if valid;
4. a generated `00:08:22:xx:xx:xx` address that changes on every boot.

`p2p0` and `ap0` use the same address with the locally administered bit set
(and one more bit flipped for the second interface).

## Devicetree

The chip sits on MSDC2 (`mmc@11170000`, pins 68-73). PMU_EN is GPIO24 and is
modelled, as in the stock devicetree, as the enable GPIO of the fixed
regulator that supplies `vmmc`; the chip needs 500 ms after PMU_EN rises.
`mt76x8_pmu_en_gpio`/`mt76x8_pmu_en_delay` of the stock tree were only used by
the vendor drivers to pulse PMU_EN low for 5 ms on a chip reset; with the
regulator the MMC core does that (`off-on-delay-us`).

    wifi_3v3: regulator-wifi-3v3 {
            compatible = "regulator-fixed";
            regulator-name = "wifi_3v3";
            regulator-min-microvolt = <3300000>;
            regulator-max-microvolt = <3300000>;
            gpio = <&pio 24 GPIO_ACTIVE_HIGH>;
            enable-active-high;
            startup-delay-us = <500000>;
            off-on-delay-us = <5000>;
            pinctrl-names = "default";
            pinctrl-0 = <&wifi_pmu_en_pins>;
    };

    &pio {
            wifi_pmu_en_pins: wifi-pmu-en-pins {
                    pins-pmu-en {
                            pinmux = <MT8516_PIN_24_EINT24__FUNC_GPIO24>;
                            bias-disable;
                    };
            };

            mmc2_pins_default: mmc2-default-pins {
                    pins-cmd-dat {
                            pinmux = <MT8516_PIN_70_MSDC2_DAT0__FUNC_MSDC2_DAT0>,
                                     <MT8516_PIN_71_MSDC2_DAT1__FUNC_MSDC2_DAT1>,
                                     <MT8516_PIN_72_MSDC2_DAT2__FUNC_MSDC2_DAT2>,
                                     <MT8516_PIN_73_MSDC2_DAT3__FUNC_MSDC2_DAT3>,
                                     <MT8516_PIN_68_MSDC2_CMD__FUNC_MSDC2_CMD>;
                            input-enable;
                            drive-strength = <8>;
                            bias-pull-up = <MTK_PUPD_SET_R1R0_10>;
                    };

                    pins-clk {
                            pinmux = <MT8516_PIN_69_MSDC2_CLK__FUNC_MSDC2_CLK>;
                            drive-strength = <8>;
                            bias-pull-down = <MTK_PUPD_SET_R1R0_10>;
                    };
            };

            /* DAT1 as GPIO71/EINT: in-band SDIO wake-up while suspended */
            mmc2_pins_eint: mmc2-eint-pins {
                    pins-dat1 {
                            pinmux = <MT8516_PIN_71_MSDC2_DAT1__FUNC_GPIO71>;
                            input-enable;
                            bias-pull-up = <MTK_PUPD_SET_R1R0_10>;
                    };
            };
    };

    &mmc2 {
            /delete-property/ interrupts;
            interrupts-extended = <&sysirq GIC_SPI 109 IRQ_TYPE_LEVEL_LOW>,
                                  <&pio 71 IRQ_TYPE_LEVEL_LOW>;
            interrupt-names = "msdc", "sdio_wakeup";
            pinctrl-names = "default", "state_uhs", "state_eint";
            pinctrl-0 = <&mmc2_pins_default>;
            pinctrl-1 = <&mmc2_pins_default>;
            pinctrl-2 = <&mmc2_pins_eint>;
            bus-width = <4>;
            max-frequency = <200000000>;
            cap-sd-highspeed;
            sd-uhs-sdr50;
            sd-uhs-sdr104;
            cap-sdio-irq;
            keep-power-in-suspend;
            wakeup-source;
            non-removable;
            no-sd;
            no-mmc;
            vmmc-supply = <&wifi_3v3>;
            #address-cells = <1>;
            #size-cells = <0>;
            status = "okay";

            wifi: wifi@1 {
                    reg = <1>;
                    /* local-mac-address is filled in by the bootloader */
            };
    };

Notes:

- `keep-power-in-suspend` is required: the driver asks for `MMC_PM_KEEP_POWER`
  on suspend. `cap-sdio-irq` is required for the in-band interrupt.
- The `sdio_wakeup` interrupt and `state_eint` are only needed for wake on
  WLAN/Bluetooth. Leave the three lines (`interrupts-extended`,
  `interrupt-names`, `pinctrl-2`/`state_eint`) out to keep the node minimal.
- `wifi@1` assumes that WLAN is SDIO function 1 (Bluetooth: function 2).
  Check `/sys/bus/sdio/devices/*/device`: `0x7608` is WLAN, `0x7668` is
  Bluetooth. A wrong `reg` is harmless, the MAC address is then taken from
  the next source.
- No `mmc-pwrseq-simple` is needed. Do not describe GPIO24 both as the
  regulator enable and as a pwrseq reset line.
- There is deliberately no `vqmmc-supply`: the stock tree describes none on
  this port and the MSDC2 pads are on the fixed 1.8 V rail.

## Bluetooth coexistence

WLAN and Bluetooth are two SDIO functions of one card (`037a:7608` and
`037a:7668`) and are driven by two independent drivers with their own SDIO
interrupt handlers; nothing in either driver claims the other function.

Both need the same ROM patch and both ask the chip for the patch semaphore
first, so the patch is downloaded once by whoever comes first:

- WLAN: `wlanDownloadPatch()` loads `mt7668_patch_e2_hdr.bin`, then calls
  `wlanPatchIsDownloaded()`, which polls the semaphore for up to 5 s while
  somebody else is downloading, and skips the download when it is done.
- Bluetooth: `mt76xx_setup()` in `btmtksdio.c` sends `BTMTK_WMT_SEMAPHORE` and
  skips `btmtk_setup_firmware()` when the patch is reported as present.

The mainline Bluetooth driver treats every semaphore answer other than "you
own it" as "patch present", including "somebody else is downloading right
now" (the vendor Bluetooth driver waits and retries in that case). Bringing
`hci0` up while the WLAN probe is in the middle of the patch download can
therefore fail. The `softdep` above avoids the window: the WLAN probe
downloads the patch and both RAM images synchronously before `btmtksdio` is
loaded.

A whole-chip reset always affects both functions. The WLAN driver resets
through `mmc_hw_reset()`; while Bluetooth is bound the MMC core removes and
re-detects the card, so both drivers are re-probed. The cross-driver core dump
handshake of the vendor drivers is gone.

## Changes against the vendor driver

- SDIO only; USB/PCIe/eHPI HIFs, the prealloc module, Android build files and
  a prebuilt 32-bit object were removed.
- Removed: procfs/debugfs (`gl_proc.c`), MET tracing, Amazon metrics, the
  Amazon thermal sensor glue (`CFG_GET_TEMPURATURE`), the platform-device
  wrapper (`mediatek,mt76x8_wifi_sdio`), IDME access, the persistent
  reset-reason counters. Android wake locks stay compiled out
  (`CONFIG_ANDROID`).
- Chip reset (`gl_rst.c`) rewritten on top of `mmc_hw_reset()`; it no longer
  uses `kallsyms_lookup_name()`, `sdio_reset_comm()` or the vendor Bluetooth
  driver's hooks.
- cfg80211: current `cfg80211_ops` prototypes (wdev/link id/radio index
  arguments, cookies assigned by cfg80211, `update_mgmt_frame_registrations`,
  `cfg80211_ap_update`), `cfg80211_roam_info`, `link_sta_params`,
  `cfg80211_register_netdevice()` for interfaces created through nl80211,
  vendor command policies.
- Kernel API updates: `timer_setup()`/`timer_container_of()`,
  `kernel_read()`/`kernel_write()` instead of `set_fs()`, `netif_rx()`,
  `eth_hw_addr_set()`, `priv_destructor`, `ndo_siocdevprivate`,
  `sched_set_fifo()`, no `strncpy()`.

## Known limitations

- Wireless extensions are unavailable (the kernel has `CONFIG_WEXT_CORE`
  off), so wext events are dropped and `iwpriv` does not work.
- The wiphys are still registered at module load, before the card is probed,
  and get their parent device assigned afterwards, as in the vendor driver.
- DFS master code calls `cfg80211_ch_switch_notify()`/`cfg80211_cac_event()`
  without holding the wiphy mutex.
- Code for kernels older than v7.3 is still present behind
  `KERNEL_VERSION()` conditionals.
- Remaining compiler warnings are all in vendor code: missing prototypes,
  empty `if` bodies from compiled-out debug macros, and a few
  fallthrough/indentation warnings.
