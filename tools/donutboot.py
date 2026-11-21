#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pack and unpack Android boot images in the layout Amazon LK on Donut
(Echo Dot 3rd gen, MT8516) accepts. Fork of lidar-mainline's
tools/lidarboot.py (same SoC, same LK rule-set) with the board compatible
parameterized and two flash-target variants.

Layout (verified against the stock NS6529 boot image in the 2019 OTA
payload, ota-donut/2019/ota_out/boot.img):
  - Android boot image header v0, page size 2048
  - kernel = 512-byte MediaTek "KERNEL" header + gzip(Image) + concatenated DTBs
    LK gunzips the payload to kernel_addr and picks the appended DTB whose
    FIRST root compatible exactly matches the board (the stock image carries
    five: amzn,donut-{proto,hvt,evt,dvt,pvt}, retail boots the proto one),
    copying it to tags_addr (max 0x80000 bytes).
  - ramdisk: plain gzip cpio, no MediaTek header, REQUIRED (an empty cpio
    works; an empty --ramdisk file is rejected)
  - cmdline MUST start with "bootopt=64S3,32N2,64N2" or LK will not jump to a
    64-bit image (the packer always prepends it).

Two flash-target variants, selected with --variant:

  boot (default)       the stock boot slots (p10/p11). Ramdisk load addr
                       0x44000000, the stock header value. The image must
                       carry a DTB whose first root compatible is exactly
                       amzn,donut-proto: the rooted unit is NS6529/proto-era,
                       and LK matches the first compatible exactly.

  swdl                 the 32 MB swdl partition (p12) patched LK boots as
                       recovery — kaeru's mt8516-donut.h names RECOVERY_
                       PARTITION "swdl", and techo5-dot/kamakiri-donut run
                       TWRP/Linux there while boot_a/boot_b stay stock.
                       Same Android boot v0 + MTK header layout at different
                       addresses: techo5's dotimage.py pins the ramdisk load
                       addr at 0x43478000 (TWRP's address — the stock
                       0x44000000 leaves under 4 MB below ram_console/pstore
                       at 0x44400000, which no real initramfs fits), and the
                       image is dd'ed raw into the 32 MB partition.
"""
import argparse
import gzip
import hashlib
import struct
import sys
import zlib

BOOT_MAGIC = b"ANDROID!"
MTK_MAGIC = 0x58881688
MTK_HDR_SIZE = 512
FDT_MAGIC = b"\xd0\x0d\xfe\xed"
PAGE = 2048
KERNEL_ADDR = 0x40080000
SECOND_ADDR = 0x40F00000
TAGS_ADDR = 0x4E000000
LK_DTB_MAX = 0x80000
BOOTOPT = "bootopt=64S3,32N2,64N2"
# LK reads the Image header's image_size to place the decompressed kernel; the
# region [KERNEL_ADDR, ATF_BASE) must hold it.
ATF_BASE = 0x43000000
# lidar's build.sh bootimg default; also the serial console of this board
DEFAULT_CMDLINE = "console=ttyS0,921600n8 earlycon=uart8250,mmio32,0x11005000"

# Boards: every packed DTB must lead with <prefix><rev> (LK matches the first
# root compatible exactly, per rev). boot_exact is the revision the variant
# "boot" image must carry for the unit this project targets (the rooted donut
# is proto/NS6529-era); None keeps lidarboot's any-rev rule.
BOARDS = {
    "donut": {"prefix": "amzn,donut-", "boot_exact": "amzn,donut-proto"},
    "lidar": {"prefix": "amzn,lidar-", "boot_exact": None},
}

VARIANTS = {
    "boot": {
        "ramdisk_addr": 0x44000000,  # stock donut boot-slot header value
        # TODO(M0): lidar's boot-slot size; confirm donut's own from the
        # unit's GPT before flashing anything near this size
        "max_size": 16 << 20,
    },
    "swdl": {
        "ramdisk_addr": 0x43478000,  # TWRP's address (techo5 dotimage.py)
        "max_size": 32 << 20,  # swdl is p12, 32 MB (techo5; system_a follows)
    },
}


def mtk_header(name: bytes, size: int) -> bytes:
    hdr = struct.pack("<II32s", MTK_MAGIC, size, name)
    return hdr + b"\xff" * (MTK_HDR_SIZE - len(hdr))


def pad(data: bytes) -> bytes:
    return data + b"\0" * (-len(data) % PAGE)


def empty_cpio_gz() -> bytes:
    # newc trailer only; LK insists on a ramdisk being present
    name = b"TRAILER!!!\0"
    hdr = b"070701" + b"".join(b"%08X" % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    body = hdr + name
    body += b"\0" * (-len(body) % 4)
    body += b"\0" * (-len(body) % 512)
    return gzip.compress(body, mtime=0)


def dtb_compatibles(dtb: bytes) -> list[str]:
    """Return the root node 'compatible' strings of a flattened DT."""
    if dtb[:4] != FDT_MAGIC:
        raise ValueError("not an FDT")
    off_struct, off_strings = struct.unpack_from(">II", dtb, 8)
    pos = off_struct
    tag = struct.unpack_from(">I", dtb, pos)[0]
    if tag != 1:
        raise ValueError("FDT does not start with BEGIN_NODE")
    pos += 4
    end = dtb.index(b"\0", pos)
    pos = (end + 4) & ~3
    while True:
        tag = struct.unpack_from(">I", dtb, pos)[0]
        if tag == 4:  # NOP
            pos += 4
            continue
        if tag != 3:  # first child or END: root has no compatible
            return []
        length, nameoff = struct.unpack_from(">II", dtb, pos + 4)
        name = dtb[off_strings + nameoff:dtb.index(b"\0", off_strings + nameoff)]
        val = dtb[pos + 12:pos + 12 + length]
        if name == b"compatible":
            return [s.decode() for s in val.rstrip(b"\0").split(b"\0")]
        pos = (pos + 12 + length + 3) & ~3


def check_arm64_image(image: bytes) -> None:
    if image[0x38:0x3C] != b"ARM\x64":
        raise SystemExit("kernel payload is not an arm64 Image (missing ARM\\x64 magic)")
    image_size = struct.unpack_from("<Q", image, 16)[0] or len(image)
    if KERNEL_ADDR + image_size > ATF_BASE:
        raise SystemExit(
            f"decompressed image ({image_size:#x} bytes incl. BSS) would overlap "
            f"BL31 at {ATF_BASE:#x}")


def pack(args) -> None:
    board = BOARDS[args.board]
    variant = VARIANTS[args.variant]
    image = open(args.kernel, "rb").read()
    if image[:2] == b"\x1f\x8b":
        image = gzip.decompress(image)
    check_arm64_image(image)

    dtbs = []
    for path in args.dtb:
        d = open(path, "rb").read()
        if len(d) > LK_DTB_MAX:
            raise SystemExit(f"{path}: {len(d)} bytes exceeds LK limit {LK_DTB_MAX:#x}")
        compat = dtb_compatibles(d)
        if not compat or not compat[0].startswith(board["prefix"]):
            raise SystemExit(
                f"{path}: first root compatible of {compat} is not {board['prefix']}<rev>; "
                f"LK will not select it")
        dtbs.append(d)
    if args.variant == "boot" and board["boot_exact"] and \
            not any(dtb_compatibles(d)[0] == board["boot_exact"] for d in dtbs):
        raise SystemExit(
            f"no DTB with first root compatible {board['boot_exact']} (the rooted unit's "
            f"board rev); LK would find no DTB to boot")

    payload = gzip.compress(image, compresslevel=9, mtime=0) + b"".join(dtbs)
    kernel = mtk_header(b"KERNEL", len(payload)) + payload
    if args.ramdisk:
        ramdisk = open(args.ramdisk, "rb").read()
        if not ramdisk:
            raise SystemExit(f"{args.ramdisk}: ramdisk is empty; LK requires one (an empty cpio works)")
    else:
        ramdisk = empty_cpio_gz()

    # accept a cmdline copied from `unpack`, which already carries BOOTOPT
    extra = args.cmdline.strip()
    if extra == BOOTOPT or extra.startswith(BOOTOPT + " "):
        extra = extra[len(BOOTOPT):].lstrip()
    cmdline = BOOTOPT + (" " + extra if extra else "")
    if len(cmdline) >= 512:
        raise SystemExit("cmdline too long for v0 header (512 bytes)")

    max_size = args.max_size if args.max_size else variant["max_size"]
    if max_size > variant["max_size"]:
        raise SystemExit(
            f"--max-size {max_size} exceeds the {args.variant} image bound of "
            f"{variant['max_size']} (swdl is a 32 MB partition; the boot-slot bound follows "
            f"lidar's, re-check at M0)")

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))

    hdr = struct.pack(
        "<8s10I16s512s32s1024s",
        BOOT_MAGIC, len(kernel), KERNEL_ADDR, len(ramdisk), variant["ramdisk_addr"],
        0, SECOND_ADDR, TAGS_ADDR, PAGE, 0, 0, b"",
        cmdline.encode(), sha.digest(), b"")
    out = pad(hdr) + pad(kernel) + pad(ramdisk)
    if len(out) > max_size:
        raise SystemExit(f"image is {len(out)} bytes; {args.variant} holds {max_size}")
    open(args.out, "wb").write(out)
    print(f"{args.out}: {len(out)} bytes, {args.variant} variant, kernel {len(kernel)}, "
          f"ramdisk {len(ramdisk)}, {len(dtbs)} dtb(s)")


def unpack(args) -> None:
    b = open(args.image, "rb").read()
    if b[:8] != BOOT_MAGIC:
        raise SystemExit("not an Android boot image")
    ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack_from("<10I", b, 8)
    cmdline = b[0x40:0x240].split(b"\0")[0].decode()
    variant = next((v for v, s in VARIANTS.items() if s["ramdisk_addr"] == ra), None)
    print(f"kernel {ks:#x}@{ka:#x} ramdisk {rs:#x}@{ra:#x} tags {ta:#x} page {ps} v{hv}"
          + (f" (ramdisk addr matches {variant})" if variant else ""))
    print(f"cmdline: {cmdline}")
    k = b[ps:ps + ks]
    if struct.unpack_from("<I", k)[0] == MTK_MAGIC:
        name = k[8:40].rstrip(b"\0").decode()
        print(f"mtk header: {name} size {struct.unpack_from('<I', k, 4)[0]:#x}")
        k = k[MTK_HDR_SIZE:]
    d = zlib.decompressobj(31)
    image = d.decompress(k)
    tail = d.unused_data
    open(f"{args.prefix}Image", "wb").write(image)
    print(f"Image: {len(image)} bytes")
    i = n = 0
    while i < len(tail) and tail[i:i + 4] == FDT_MAGIC:
        size = struct.unpack_from(">I", tail, i + 4)[0]
        dtb = tail[i:i + size]
        compat = dtb_compatibles(dtb)
        open(f"{args.prefix}dtb{n}.dtb", "wb").write(dtb)
        print(f"dtb{n}: {size} bytes {compat}")
        i += size
        n += 1
    roff = ps + -(-ks // ps) * ps
    open(f"{args.prefix}ramdisk.gz", "wb").write(b[roff:roff + rs])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--kernel", required=True, help="arm64 Image (raw or gzip), e.g. u-boot.bin")
    p.add_argument("--dtb", required=True, action="append",
                   help="DTB whose first root compatible matches the board (one per rev); repeatable")
    p.add_argument("--ramdisk", help="gzip cpio; default: empty cpio (LK requires one)")
    p.add_argument("--cmdline", default=DEFAULT_CMDLINE,
                   help=f"appended after '{BOOTOPT}' (a leading copy is dropped; "
                        f"default: '{DEFAULT_CMDLINE}')")
    p.add_argument("--variant", choices=sorted(VARIANTS), default="boot",
                   help="flash target: boot slot (default) or the 32 MB swdl partition")
    p.add_argument("--board", choices=sorted(BOARDS), default="donut",
                   help="board whose DTB compatibles LK matches against")
    p.add_argument("--max-size", type=int, default=0,
                   help=f"image size bound (default and maximum: the variant's, "
                        f"{VARIANTS['boot']['max_size']} for boot, {VARIANTS['swdl']['max_size']} for swdl)")
    p.add_argument("-o", "--out", required=True)
    p.set_defaults(func=pack)
    u = sub.add_parser("unpack")
    u.add_argument("image")
    u.add_argument("--prefix", default="")
    u.set_defaults(func=unpack)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
