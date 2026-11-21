#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pack and unpack Android boot images in the layout Amazon LK on Lidar
(Echo Plus 2nd gen, MT8516) accepts.

Layout (verified against a stock boot_a dump):
  - Android boot image header v0, page size 2048
  - kernel = 512-byte MediaTek "KERNEL" header + gzip(Image) + concatenated DTBs
    LK gunzips the payload to kernel_addr and picks the appended DTB whose
    root compatible exactly matches the board (e.g. "amzn,lidar-pvt"),
    copying it to tags_addr (max 0x80000 bytes).
  - ramdisk: plain gzip cpio, no MediaTek header
  - cmdline MUST start with "bootopt=64S3,32N2,64N2" or LK will not jump to a
    64-bit image.
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
RAMDISK_ADDR = 0x44000000
SECOND_ADDR = 0x40F00000
TAGS_ADDR = 0x4E000000
LK_DTB_MAX = 0x80000
BOOTOPT = "bootopt=64S3,32N2,64N2"
# LK reads the Image header's image_size to place the decompressed kernel; the
# region [KERNEL_ADDR, ATF_BASE) must hold it.
ATF_BASE = 0x43000000


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
        if not compat or not compat[0].startswith("amzn,lidar-"):
            raise SystemExit(f"{path}: first root compatible of {compat} is not amzn,lidar-<rev>; LK will not select it")
        dtbs.append(d)

    payload = gzip.compress(image, compresslevel=9, mtime=0) + b"".join(dtbs)
    kernel = mtk_header(b"KERNEL", len(payload)) + payload
    ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else empty_cpio_gz()

    # accept a cmdline copied from `unpack`, which already carries BOOTOPT
    extra = args.cmdline.strip()
    if extra == BOOTOPT or extra.startswith(BOOTOPT + " "):
        extra = extra[len(BOOTOPT):].lstrip()
    cmdline = BOOTOPT + (" " + extra if extra else "")
    if len(cmdline) >= 512:
        raise SystemExit("cmdline too long for v0 header (512 bytes)")

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))

    hdr = struct.pack(
        "<8s10I16s512s32s1024s",
        BOOT_MAGIC, len(kernel), KERNEL_ADDR, len(ramdisk), RAMDISK_ADDR,
        0, SECOND_ADDR, TAGS_ADDR, PAGE, 0, 0, b"",
        cmdline.encode(), sha.digest(), b"")
    out = pad(hdr) + pad(kernel) + pad(ramdisk)
    if len(out) > args.max_size:
        raise SystemExit(f"image is {len(out)} bytes; boot partition holds {args.max_size}")
    open(args.out, "wb").write(out)
    print(f"{args.out}: {len(out)} bytes, kernel {len(kernel)}, ramdisk {len(ramdisk)}, {len(dtbs)} dtb(s)")


def unpack(args) -> None:
    b = open(args.image, "rb").read()
    if b[:8] != BOOT_MAGIC:
        raise SystemExit("not an Android boot image")
    ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack_from("<10I", b, 8)
    cmdline = b[0x40:0x240].split(b"\0")[0].decode()
    print(f"kernel {ks:#x}@{ka:#x} ramdisk {rs:#x}@{ra:#x} tags {ta:#x} page {ps} v{hv}")
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
    p.add_argument("--dtb", required=True, action="append", help="DTB with amzn,lidar-<rev> root compatible; repeatable")
    p.add_argument("--ramdisk", help="gzip cpio; default: empty cpio")
    p.add_argument("--cmdline", default="", help=f"appended after '{BOOTOPT}' (a leading copy is dropped)")
    p.add_argument("--max-size", type=int, default=16 << 20)
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
