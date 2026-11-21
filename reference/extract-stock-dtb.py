#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract a board DTB from a stock Amazon boot image and decompile it.

The Amazon/MTK boot images for donut-class devices carry a 512-byte MTK
KERNEL header (magic 0x58881688) followed by a gzip-compressed kernel
image, with one DTB per board revision appended after the gzip stream.
Amazon LK walks that list and hands the kernel the DTB whose FIRST root
compatible matches the board it detected (see tools/lidarboot.py for the
packing side of the same convention).

This script parses the Android boot image v0 header, locates the appended
FDT chain inside the kernel section, picks the DTB for a given board
compatible and decompiles it with dtc. Pure stdlib apart from the dtc
subprocess.

Usage:
    ./extract-stock-dtb.py [bootimg] [-o out.dts] [--board amzn,donut-proto]

Default boot image: the 2019 donut OTA boot.img this repository was
verified against (ota-donut/2019/ota_out/boot.img, resolved relative to
this script so it works from any cwd).
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FDT_MAGIC = b"\xd0\x0d\xfe\xed"
ANDROID_MAGIC = b"ANDROID!"
DEFAULT_BOOTIMG = Path(__file__).resolve().parents[3] / "ota-donut/2019/ota_out/boot.img"
DEFAULT_BOARD = "amzn,donut-proto"


def parse_boot_v0(data: bytes) -> dict:
    """Parse the fixed part of an Android boot image v0 header."""
    magic, kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, \
        second_size, second_addr, tags_addr, page_size, header_version \
        = struct.unpack_from("<8s9I", data, 0)
    if magic != ANDROID_MAGIC:
        raise SystemExit(f"not an Android boot image (magic {magic!r})")
    if header_version > 2:
        raise SystemExit(f"boot image header v{header_version} not handled")
    return {
        "kernel_size": kernel_size,
        "kernel_addr": kernel_addr,
        "ramdisk_size": ramdisk_size,
        "page_size": page_size,
    }


def fdt_valid(data: bytes, off: int) -> int | None:
    """Return the DTB total size if a structurally sane FDT starts at off."""
    if data[off:off + 4] != FDT_MAGIC or off + 40 > len(data):
        return None
    totalsize, off_struct, _off_strings, off_mem, version, _last_comp, \
        _cpuid, _sz_strings, _sz_struct = struct.unpack_from(">9I", data, off + 4)
    if not 0 < totalsize <= len(data) - off:
        return None
    if not 0 < off_struct < totalsize or not 0 < off_mem < totalsize:
        return None
    if version < 16:
        return None
    return totalsize


def dtb_compatibles(dtb: bytes) -> list[str]:
    """Root node 'compatible' strings of a flattened DT (lidarboot.py logic)."""
    off_struct, off_strings = struct.unpack_from(">II", dtb, 8)
    pos = off_struct
    if struct.unpack_from(">I", dtb, pos)[0] != 1:  # FDT_BEGIN_NODE
        raise SystemExit("malformed DTB: no root node")
    pos += 4 + len(dtb[pos + 4:dtb.index(b"\0", pos + 4)].decode()) + 1
    pos = (pos + 3) & ~3
    compat = []
    while True:
        tag = struct.unpack_from(">I", dtb, pos)[0]
        if tag != 3:  # FDT_PROP: root has no (more) compatible
            break
        length, nameoff = struct.unpack_from(">II", dtb, pos + 4)
        name = dtb[off_strings + nameoff:dtb.index(b"\0", off_strings + nameoff)]
        if name == b"compatible":
            compat = dtb[pos + 12:pos + 12 + length].split(b"\0")
            compat = [c.decode() for c in compat if c]
        pos += 12 + ((length + 3) & ~3)
    return compat


def dtb_model(dtb: bytes) -> str | None:
    """Root node 'model' string of a flattened DT."""
    off_struct, off_strings = struct.unpack_from(">II", dtb, 8)
    pos = off_struct + 4  # FDT_BEGIN_NODE tag; root node name is empty
    pos = (pos + 1 + 3) & ~3  # name "" + NUL, 4-byte aligned
    tag = struct.unpack_from(">I", dtb, pos)[0]
    while tag == 3:
        length, nameoff = struct.unpack_from(">II", dtb, pos + 4)
        name = dtb[off_strings + nameoff:dtb.index(b"\0", off_strings + nameoff)]
        if name == b"model":
            return dtb[pos + 12:pos + 12 + length].split(b"\0")[0].decode()
        pos += 12 + ((length + 3) & ~3)
        tag = struct.unpack_from(">I", dtb, pos)[0]
    return None


def find_dtbs(kernel: bytes) -> list[tuple[int, bytes]]:
    """Locate the appended DTB chain in the kernel section of the image.

    The DTBs sit after the gzip stream (MTK kernel: header + gzip + DTBs),
    but gzip data can contain FDT magic bytes by chance, so every hit is
    validated as a full FDT header and the chain is walked by totalsize.
    """
    dtbs = []
    off = 0
    while off < len(kernel):
        hit = kernel.find(FDT_MAGIC, off)
        if hit < 0:
            break
        size = fdt_valid(kernel, hit)
        if size:
            dtbs.append((hit, kernel[hit:hit + size]))
            off = hit + size  # next DTB follows this one
        else:
            off = hit + 4
    return dtbs


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("bootimg", nargs="?", type=Path, default=DEFAULT_BOOTIMG,
                    help=f"stock boot image (default: {DEFAULT_BOOTIMG})")
    ap.add_argument("-o", "--out", type=Path,
                    default=Path(__file__).resolve().parent / "donut-stock-ns6529.dts")
    ap.add_argument("--board", default=DEFAULT_BOARD,
                    help=f"first root compatible of the DTB to extract (default: {DEFAULT_BOARD})")
    args = ap.parse_args()

    data = args.bootimg.read_bytes()
    hdr = parse_boot_v0(data)
    print(f"{args.bootimg}: v0 header, page {hdr['page_size']}, "
          f"kernel {hdr['kernel_size']} bytes @ 0x{hdr['kernel_addr']:08x}")

    kernel = data[hdr["page_size"]:hdr["page_size"] + hdr["kernel_size"]]
    dtbs = find_dtbs(kernel)
    if not dtbs:
        raise SystemExit("no appended DTB found in the kernel section")

    chosen = None
    for i, (off, dtb) in enumerate(dtbs):
        compat = dtb_compatibles(dtb)
        model = dtb_model(dtb)
        print(f"dtb{i}: offset 0x{hdr['page_size'] + off:x} "
              f"({off} in kernel blob), {len(dtb)} bytes, "
              f"model={model!r}, compatible={compat}")
        if chosen is None and compat and compat[0] == args.board:
            chosen = (i, dtb, compat, model)

    if chosen is None:
        boards = ", ".join(dtb_compatibles(d)[0] for _, d in dtbs)
        raise SystemExit(f"no DTB with first compatible {args.board!r} "
                         f"(found: {boards})")

    i, dtb, compat, model = chosen
    print(f"selected dtb{i} for {args.board}: model={model!r}, compatible={compat}")

    with tempfile.NamedTemporaryFile(suffix=".dtb", delete=False) as f:
        f.write(dtb)
        tmp = Path(f.name)
    try:
        out = subprocess.run(
            ["dtc", "-I", "dtb", "-O", "dts", "-o", str(args.out), str(tmp)],
            capture_output=True, text=True)
    finally:
        tmp.unlink()
    if out.stderr:
        sys.stderr.write(out.stderr)  # dtc notes phandle warnings on stock dtbs
    if out.returncode:
        raise SystemExit(f"dtc failed with {out.returncode}")
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
