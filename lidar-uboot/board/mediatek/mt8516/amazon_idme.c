// SPDX-License-Identifier: GPL-2.0+
/*
 * Amazon IDME (per-device manufacturing data) hand-over to the OS.
 *
 * Amazon LK adds an /idme node to the devicetree it passes in x0. Each
 * IDME field is a child node whose "value" property holds the raw field,
 * for MAC addresses 12 ASCII hex digits without separators. U-Boot runs
 * from its own devicetree, so the fields the OS needs are copied over to
 * the OS devicetree here.
 */

#include <fdt_support.h>
#include <hexdump.h>
#include <init.h>
#include <mapmem.h>
#include <net.h>
#include <linux/libfdt.h>

#define IDME_WIFI_MAC	"/idme/mac_addr"
#define OS_WIFI_NODE	"/soc/mmc@11170000/wifi@1"

static int idme_get_mac(const void *idme, const char *path, u8 *mac)
{
	const char *val;
	int node, len;

	node = fdt_path_offset(idme, path);
	if (node < 0)
		return node;

	val = fdt_getprop(idme, node, "value", &len);
	if (!val || len < ARP_HLEN * 2)
		return -FDT_ERR_NOTFOUND;
	if (len > ARP_HLEN * 2 && val[ARP_HLEN * 2])
		return -FDT_ERR_BADVALUE;
	if (hex2bin(mac, val, ARP_HLEN) || !is_valid_ethaddr(mac))
		return -FDT_ERR_BADVALUE;

	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	phys_addr_t addr = get_prev_bl_fdt_addr();
	const void *idme;
	u8 mac[ARP_HLEN];
	int node, ret;

	if (!addr)
		return 0;

	idme = map_sysmem(addr, 0);
	/* LK copies its devicetree with a 0x80000 byte limit */
	if (fdt_check_header(idme) || fdt_totalsize(idme) > 0x80000)
		return 0;

	/* An address already in the OS devicetree wins */
	node = fdt_path_offset(blob, OS_WIFI_NODE);
	if (node < 0 || fdt_getprop(blob, node, "local-mac-address", NULL))
		return 0;

	if (idme_get_mac(idme, IDME_WIFI_MAC, mac))
		return 0;

	ret = fdt_setprop(blob, node, "local-mac-address", mac, sizeof(mac));
	if (ret)
		printf("idme: cannot set Wi-Fi MAC address: %s\n",
		       fdt_strerror(ret));

	return 0;
}
