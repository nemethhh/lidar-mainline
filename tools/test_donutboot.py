#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for tools/donutboot.py, the LK payload packager for Donut.

Every enforcement the plan demands is pinned here against the byte layout
verified from the stock NS6529 donut boot image (Android boot v0, page 2048,
kernel 0x40080000, second 0x40f00000, tags 0x4e000000, MTK KERNEL header
0x58881688 of 512 bytes, cmdline starting bootopt=64S3,32N2,64N2, five
appended DTBs amzn,donut-{proto,hvt,evt,dvt,pvt} with amzn,donut as fallback,
ramdisk plain gzip at 0x44000000 boot / 0x43478000 swdl).

The stock-image ground-truth tests run when ota-donut/2019/ota_out/boot.img
is present (four directories up from this file); set DONUT_STOCK_BOOT=<path>
to point elsewhere. Everything else builds its own blobs.

    python3 tools/test_donutboot.py      # or: python3 tools/test_donutboot.py -v
"""
import gzip
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import donutboot  # noqa: E402

BOOTOPT = "bootopt=64S3,32N2,64N2"
DEFAULT_CMDLINE = "console=ttyS0,921600n8 earlycon=uart8250,mmio32,0x11005000"
STOCK_BOOT = Path(os.environ.get(
    "DONUT_STOCK_BOOT",
    Path(__file__).resolve().parents[3] / "ota-donut/2019/ota_out/boot.img"))
PAGE = 2048
CMDLINE_OFF = 0x40
ID_OFF = 0x240


# ---------------------------------------------------------------- helpers

def build_dtb(compatibles, model="Amazon Echo Dot 3rd gen (donut)"):
    """A minimal but well-formed FDT whose root node carries a model
    property (before compatible, to exercise the parser's walk) and the
    given compatible strings."""
    strings = b"compatible\0model\0"
    model_off = strings.index(b"model\0")
    props = b""
    model_val = model.encode() + b"\0"
    props += struct.pack(">III", 3, len(model_val), model_off) + model_val
    props += b"\0" * (-len(props) % 4)
    comp_val = b"\0".join(c.encode() for c in compatibles) + b"\0"
    props += struct.pack(">III", 3, len(comp_val), 0) + comp_val
    props += b"\0" * (-len(props) % 4)
    body = struct.pack(">I", 1)  # FDT_BEGIN_NODE
    body += b"\0" + b"\0" * 3    # root's empty name, NUL-terminated, padded
    body += props
    body += struct.pack(">II", 2, 9)  # FDT_END_NODE, FDT_END
    rsvmap = b"\0" * 16
    off_rsv = 40
    off_struct = off_rsv + len(rsvmap)
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    blob = (struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings,
                        off_rsv, 17, 16, 0, len(strings), len(body))
            + rsvmap + body + strings)
    assert len(blob) == total
    assert donutboot.dtb_compatibles(blob) == list(compatibles)
    return blob


def build_dtb_with_props(props):
    """A well-formed FDT whose root carries exactly the given (name, value)
    properties — for DTBs with no compatible at all."""
    strings = b"".join(n.encode() + b"\0" for n, _ in props)
    body = struct.pack(">I", 1) + b"\0" + b"\0" * 3
    for name, val in props:
        off = strings.index(name.encode() + b"\0")
        body += struct.pack(">III", 3, len(val), off) + val
        body += b"\0" * (-len(body) % 4)
    body += struct.pack(">II", 2, 9)
    rsv = b"\0" * 16
    off_struct = 40 + len(rsv)
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    return (struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings,
                        40, 17, 16, 0, len(strings), len(body))
            + rsv + body + strings)


def build_kernel(size=0x2000, image_size=None):
    """An arm64-Image-shaped blob: ARM\\x64 magic at 0x38, image_size at
    0x10 (u-boot.bin carries the same header, so it stands in for both)."""
    body = bytes((i * 7 + 0x5A) & 0xFF for i in range(size))
    hdr = bytearray(0x100)
    struct.pack_into("<Q", hdr, 0x10, image_size if image_size is not None else 0x100 + size)
    hdr[0x38:0x3C] = b"ARM\x64"
    return bytes(hdr) + body


def build_ramdisk(data=b"donut-initramfs-" * 64):
    return gzip.compress(data + b"\0", mtime=0)


def run(*argv, ok=True):
    r = subprocess.run([sys.executable, str(TOOLS / "donutboot.py"), *argv],
                       capture_output=True, text=True)
    if ok:
        assert r.returncode == 0, f"donutboot.py failed: {r.stderr}"
    else:
        assert r.returncode != 0, f"donutboot.py should have failed: {r.stdout}"
        assert (r.stderr + r.stdout).strip(), "failure printed nothing"
    return r


def parse_bootimg(path, strict=True):
    """Parse a packed image independently of donutboot (roundtrip evidence).
    strict=False for stock images, which carry a trailing Amazon signature
    block (ASN.1 DER) of up to 4096 bytes after the page-aligned sections."""
    b = Path(path).read_bytes()
    assert b[:8] == b"ANDROID!", "bad magic"
    ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack_from("<10I", b, 8)
    assert hv == 0
    cmdline = b[CMDLINE_OFF:CMDLINE_OFF + 512].split(b"\0")[0].decode()
    kernel = b[ps:ps + ks]
    roff = ps + -(-ks // ps) * ps
    ramdisk = b[roff:roff + rs]
    end = roff + -(-rs // ps) * ps
    if strict:
        assert len(b) == end, "sections page-aligned, nothing else"
    else:
        assert 0 <= len(b) - end <= 4096, "only a signature block may follow"
    return dict(b=b, ks=ks, ka=ka, rs=rs, ra=ra, ss=ss, sa=sa, ta=ta, ps=ps,
                osv=osv, cmdline=cmdline, id=b[ID_OFF:ID_OFF + 20],
                kernel=kernel, ramdisk=ramdisk)


class Blobs(unittest.TestCase):
    """A tmpdir with kernel/dtb/ramdisk inputs, as the CLI would see them."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="donutboot-test-"))
        self.kernel = self.dir / "kernel.img"
        self.kernel.write_bytes(build_kernel())
        self.ramdisk = self.dir / "ramdisk.gz"
        self.ramdisk.write_bytes(build_ramdisk())
        for rev in ("proto", "hvt", "evt", "dvt", "pvt"):
            (self.dir / f"donut-{rev}.dtb").write_bytes(
                build_dtb([f"amzn,donut-{rev}", "amzn,donut"]))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.dir, ignore_errors=True)

    def pack(self, dtb=("donut-proto.dtb",), kernel=None, ramdisk="ramdisk.gz",
             cmdline=DEFAULT_CMDLINE, extra=(), out="boot.img"):
        argv = ["pack", "--kernel", str(kernel or self.kernel)]
        for d in dtb:
            argv += ["--dtb", str(self.dir / d)]
        if ramdisk:
            argv += ["--ramdisk", str(self.dir / ramdisk)]
        if cmdline is not None:
            argv += ["--cmdline", cmdline]
        argv += list(extra) + ["-o", str(self.dir / out)]
        run(*argv)
        return self.dir / out

    def parse(self, path, strict=True):
        return parse_bootimg(path, strict=strict)

    def split_mtk(self, kernel):
        magic, size = struct.unpack_from("<II", kernel, 0)
        assert magic == 0x58881688, hex(magic)
        name = kernel[8:40].rstrip(b"\0")
        assert kernel[40:512] == b"\xff" * (512 - 40), "0xff padding after the name"
        payload = kernel[512:512 + size]
        assert len(payload) == size, "MTK size field covers the whole payload"
        assert len(kernel) == 512 + size, "kernel section is exactly header + payload"
        return name, payload

    def split_payload(self, payload):
        d = zlib.decompressobj(31)
        image = d.decompress(payload)
        tail, dtbs, i = d.unused_data, [], 0
        while i < len(tail) and tail[i:i + 4] == b"\xd0\x0d\xfe\xed":
            size = struct.unpack_from(">I", tail, i + 4)[0]
            dtbs.append(tail[i:i + size])
            i += size
        assert i == len(tail), "payload ends exactly after the DTBs"
        return image, dtbs


# ---------------------------------------------------------------- header bytes

class TestHeaderBytes(Blobs):

    def test_boot_header_bytes(self):
        h = self.parse(self.pack())
        self.assertEqual(h["ka"], 0x40080000, "entry 0x40080000 (EL2), stock value")
        self.assertEqual(h["ra"], 0x44000000, "boot ramdisk addr is the stock header value")
        self.assertEqual(h["sa"], 0x40F00000)
        self.assertEqual(h["ta"], 0x4E000000, "LK copies the selected DTB to tags_addr")
        self.assertEqual((h["ss"], h["osv"], h["ps"]), (0, 0, 2048))
        self.assertEqual(h["cmdline"], BOOTOPT + " " + DEFAULT_CMDLINE,
                         "default cmdline appended after bootopt")
        # mkbootimg-style id: sha1 over kernel, ramdisk, second(empty), each + size
        sha = hashlib.sha1()
        for blob in (h["kernel"], h["ramdisk"], b""):
            sha.update(blob)
            sha.update(struct.pack("<I", len(blob)))
        self.assertEqual(h["id"], sha.digest())

    def test_swdl_header_bytes(self):
        h = self.parse(self.pack(extra=["--variant", "swdl"], out="swdl.img"))
        self.assertEqual(h["ra"], 0x43478000, "swdl ramdisk addr (TWRP's, techo5 evidence)")
        self.assertEqual(h["ka"], 0x40080000)
        self.assertEqual(h["ta"], 0x4E000000)
        self.assertEqual(h["ps"], 2048)
        self.assertEqual(h["cmdline"], BOOTOPT + " " + DEFAULT_CMDLINE)

    def test_variants_differ_only_in_ramdisk_addr(self):
        boot = self.pack(out="a.img").read_bytes()
        swdl = self.pack(extra=["--variant", "swdl"], out="s.img").read_bytes()
        self.assertEqual(len(boot), len(swdl))
        diffs = [i for i, (x, y) in enumerate(zip(boot, swdl)) if x != y]
        self.assertEqual(diffs, [21, 22, 23], "only the ramdisk_addr field differs (LSBs coincide)")


# ------------------------------------------------------- MTK header + payload

class TestMtkPayload(Blobs):

    def test_mtk_header_layout(self):
        h = self.parse(self.pack())
        name, payload = self.split_mtk(h["kernel"])
        self.assertEqual(name, b"KERNEL")
        image, dtbs = self.split_payload(payload)
        self.assertEqual(image, build_kernel(), "payload gunzips to the kernel, byte-identical")
        self.assertEqual(dtbs, [(self.dir / "donut-proto.dtb").read_bytes()])

    def test_dtb_concat_order_and_multi_dtb(self):
        revs = ("proto", "hvt", "evt", "dvt", "pvt")
        h = self.parse(self.pack(dtb=tuple(f"donut-{r}.dtb" for r in revs)))
        _, payload = self.split_mtk(h["kernel"])
        _, dtbs = self.split_payload(payload)
        self.assertEqual(dtbs, [(self.dir / f"donut-{r}.dtb").read_bytes() for r in revs],
                         "concat order preserved: all five stock revs")

    def test_gzip_kernel_input(self):
        (self.dir / "kernel.gz").write_bytes(gzip.compress(build_kernel(), mtime=0))
        raw = self.pack(out="raw.img").read_bytes()
        gz = self.pack(kernel=self.dir / "kernel.gz", out="gz.img").read_bytes()
        self.assertEqual(raw, gz, "gzip and raw kernel input pack identically")


# ------------------------------------------------------------------- ramdisk

class TestRamdisk(Blobs):

    def test_default_ramdisk_is_empty_cpio(self):
        h = self.parse(self.pack(ramdisk=None))
        self.assertGreater(h["rs"], 0, "a ramdisk is always present")
        self.assertEqual(h["ramdisk"][:2], b"\x1f\x8b")
        self.assertIn(b"TRAILER!!!", gzip.decompress(h["ramdisk"]))
        self.assertNotEqual(h["ramdisk"][:4], struct.pack("<I", 0x58881688),
                            "no MTK header on the ramdisk")

    def test_custom_ramdisk_lands_byte_identical(self):
        h = self.parse(self.pack())
        self.assertEqual(h["ramdisk"], self.ramdisk.read_bytes())

    def test_empty_ramdisk_file_rejected(self):
        (self.dir / "empty.gz").write_bytes(b"")
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "donut-proto.dtb"),
            "--ramdisk", str(self.dir / "empty.gz"),
            "-o", str(self.dir / "x.img"), ok=False)


# ------------------------------------------------------------------- cmdline

class TestCmdline(Blobs):

    def test_bootopt_prefix_always_first(self):
        for extra in (None, "", "console=ttyS0,921600n8"):
            with self.subTest(extra=extra):
                cmdline = self.parse(self.pack(cmdline=extra, out="c.img"))["cmdline"]
                self.assertTrue(cmdline == BOOTOPT or cmdline.startswith(BOOTOPT + " "))

    def test_cmdline_copied_from_unpack_not_doubled(self):
        h = self.parse(self.pack(cmdline=BOOTOPT + " console=ttyS0,921600n8"))
        self.assertEqual(h["cmdline"], BOOTOPT + " console=ttyS0,921600n8")

    def test_cmdline_too_long_rejected(self):
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "donut-proto.dtb"),
            "--cmdline", "x=" + "y" * 600,
            "-o", str(self.dir / "x.img"), ok=False)


# ------------------------------------------------------- DTB compatible rules

class TestDtbRules(Blobs):

    def test_reject_wrong_compatible(self):
        for compat in (["amzn,lidar-pvt"],        # wrong board entirely
                       ["amzn,donut"],            # no <rev>: LK matches first exactly
                       ["mediatek,mt8516-evb"]):  # not an Amazon board
            with self.subTest(compat=compat):
                (self.dir / "bad.dtb").write_bytes(build_dtb(compat))
                run("pack", "--kernel", str(self.kernel),
                    "--dtb", str(self.dir / "bad.dtb"),
                    "-o", str(self.dir / "x.img"), ok=False)

    def test_reject_dtb_without_compatible(self):
        (self.dir / "noc.dtb").write_bytes(
            build_dtb_with_props([("model", b"no compatible here\0")]))
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "noc.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)

    def test_reject_non_fdt_dtb(self):
        (self.dir / "nope.dtb").write_bytes(b"\xde\xad\xbe\xef" * 16)
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "nope.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)

    def test_reject_dtb_over_lk_limit(self):
        blob = bytearray(build_dtb(["amzn,donut-proto", "amzn,donut"]))
        blob += b"\0" * (0x80001 - len(blob))  # extra zeros past totalsize: len check must catch it
        (self.dir / "big.dtb").write_bytes(bytes(blob))
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "big.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)

    def test_boot_variant_requires_proto_dtb(self):
        # the rooted unit is proto/NS6529-era: a boot image whose DTBs are all
        # other revs has nothing LK would select
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "donut-dvt.dtb"),
            "--dtb", str(self.dir / "donut-pvt.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)
        # the stock five-rev set (proto first) packs fine
        self.pack(dtb=tuple(f"donut-{r}.dtb" for r in ("proto", "hvt", "evt", "dvt", "pvt")))

    def test_swdl_variant_accepts_any_donut_rev(self):
        # brief scopes the exact proto match to the boot variant; swdl keeps
        # the donut-family check only
        h = self.parse(self.pack(dtb=("donut-dvt.dtb",), extra=["--variant", "swdl"],
                                 out="swdl-dvt.img"))
        self.assertEqual(h["ra"], 0x43478000)

    def test_board_parameter(self):
        (self.dir / "lidar.dtb").write_bytes(build_dtb(["amzn,lidar-pvt", "amzn,lidar"]))
        h = self.parse(self.pack(dtb=("lidar.dtb",), extra=["--board", "lidar"],
                                 out="lidar.img"))
        self.assertEqual(h["ka"], 0x40080000)
        # and the same DTB is rejected for the donut board (default)
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "lidar.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)


# ------------------------------------------------------------- kernel checks

class TestKernelChecks(Blobs):

    def test_reject_kernel_without_arm64_magic(self):
        k = bytearray(build_kernel())
        k[0x38:0x3C] = b"\0\0\0\0"
        (self.dir / "bad.img").write_bytes(bytes(k))
        run("pack", "--kernel", str(self.dir / "bad.img"),
            "--dtb", str(self.dir / "donut-proto.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)

    def test_reject_kernel_overlapping_bl31(self):
        (self.dir / "big.img").write_bytes(build_kernel(image_size=0x40000000))
        run("pack", "--kernel", str(self.dir / "big.img"),
            "--dtb", str(self.dir / "donut-proto.dtb"),
            "-o", str(self.dir / "x.img"), ok=False)


# --------------------------------------------------------------- size bounds

class TestSizeBounds(Blobs):

    def test_max_size_enforced(self):
        run("pack", "--kernel", str(self.kernel),
            "--dtb", str(self.dir / "donut-proto.dtb"),
            "--max-size", "4096",
            "-o", str(self.dir / "x.img"), ok=False)

    def test_swdl_bound_is_32mb(self):
        self.assertEqual(donutboot.VARIANTS["swdl"]["max_size"], 32 << 20)
        self.assertEqual(donutboot.VARIANTS["swdl"]["ramdisk_addr"], 0x43478000)
        self.assertEqual(donutboot.VARIANTS["boot"]["ramdisk_addr"], 0x44000000)

    def test_variant_max_size_cannot_be_raised(self):
        for variant, over in (("boot", 32 << 20), ("swdl", 64 << 20)):
            with self.subTest(variant=variant):
                run("pack", "--kernel", str(self.kernel),
                    "--dtb", str(self.dir / "donut-proto.dtb"),
                    "--variant", variant, "--max-size", str(over),
                    "-o", str(self.dir / "x.img"), ok=False)


# ------------------------------------------------------- unpack + roundtrips

class TestUnpack(Blobs):

    def test_unpack_roundtrip_files(self):
        img = self.pack(dtb=("donut-proto.dtb", "donut-dvt.dtb"))
        work = self.dir / "unpacked"
        work.mkdir()
        r = subprocess.run([sys.executable, str(TOOLS / "donutboot.py"),
                            "unpack", str(img)], capture_output=True, text=True, cwd=work)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((work / "Image").read_bytes(), build_kernel())
        self.assertEqual((work / "dtb0.dtb").read_bytes(),
                         (self.dir / "donut-proto.dtb").read_bytes())
        self.assertEqual((work / "dtb1.dtb").read_bytes(),
                         (self.dir / "donut-dvt.dtb").read_bytes())
        self.assertEqual((work / "ramdisk.gz").read_bytes(), self.ramdisk.read_bytes())
        self.assertIn("mtk header: KERNEL", r.stdout)
        self.assertIn("amzn,donut-proto", r.stdout)

    def test_unpack_detects_variant(self):
        img = self.pack(extra=["--variant", "swdl"], out="s.img")
        work = self.dir / "unpacked"
        work.mkdir()
        r = subprocess.run([sys.executable, str(TOOLS / "donutboot.py"),
                            "unpack", str(img)], capture_output=True, text=True, cwd=work)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ramdisk addr matches swdl", r.stdout)

    def test_repack_own_image_byte_identical(self):
        # our gzip is deterministic (level 9, mtime 0): unpack -> repack from
        # the unpacked files reproduces the image byte for byte
        img = self.pack(out="first.img").read_bytes()
        work = self.dir / "unpacked"
        work.mkdir()
        subprocess.run([sys.executable, str(TOOLS / "donutboot.py"),
                        "unpack", str(self.dir / "first.img")],
                       check=True, capture_output=True, cwd=work)
        run("pack", "--kernel", str(work / "Image"),
            "--dtb", str(work / "dtb0.dtb"),
            "--ramdisk", str(work / "ramdisk.gz"),
            "--cmdline", BOOTOPT + " " + DEFAULT_CMDLINE,
            "-o", str(work / "again.img"))
        self.assertEqual(img, (work / "again.img").read_bytes())


# ------------------------------------------------- stock image ground truth

@unittest.skipUnless(STOCK_BOOT.exists(), f"stock image not found at {STOCK_BOOT}")
class TestStockImage(Blobs):
    """The 2019 OTA boot.img: ground truth for every layout constant."""

    @classmethod
    def setUpClass(cls):
        cls.h = parse_bootimg(STOCK_BOOT, strict=False)
        cls.cmdline = cls.h["cmdline"]

    def test_stock_layout(self):
        h = self.h
        self.assertEqual(h["ka"], 0x40080000)
        self.assertEqual(h["ra"], 0x44000000, "stock boot slot loads the ramdisk at 0x44000000")
        self.assertEqual(h["sa"], 0x40F00000)
        self.assertEqual(h["ta"], 0x4E000000)
        self.assertEqual(h["ps"], 2048)
        self.assertTrue(h["cmdline"].startswith(BOOTOPT))
        self.assertGreater(h["rs"], 0)
        self.assertEqual(h["ramdisk"][:2], b"\x1f\x8b", "plain gzip ramdisk, no MTK header")
        name, payload = self.split_mtk(h["kernel"])
        self.assertEqual(name, b"KERNEL")
        image, dtbs = self.split_payload(payload)
        self.assertEqual(image[0x38:0x3C], b"ARM\x64")
        # five appended DTBs, one per board rev; retail (and our rooted unit)
        # boots the proto one
        firsts = [donutboot.dtb_compatibles(d)[0] for d in dtbs]
        self.assertEqual(firsts, ["amzn,donut-proto", "amzn,donut-hvt",
                                  "amzn,donut-evt", "amzn,donut-dvt", "amzn,donut-pvt"])
        for d in dtbs:
            self.assertEqual(donutboot.dtb_compatibles(d)[1], "amzn,donut")

    def test_stock_roundtrip_after_decompression(self):
        # unpack -> repack is byte-identical after decompression (lidar's
        # standard): every load address, the page size, the cmdline, the
        # decompressed kernel, all five DTBs and the ramdisk match. Three
        # documented deltas remain: the gzip stream (stock's compressor is
        # not zlib-9, so kernel_size/MTK size differ), the trailing Amazon
        # signature block (up to 4096 bytes past the page-aligned sections,
        # which unlocked-unit LK does not check), and the os_version header
        # field (stock carries 0x0e000071, ours 0 — lidarboot's rule; LK
        # ignores it).
        work = self.dir / "stock-unpacked"
        work.mkdir()
        r = subprocess.run([sys.executable, str(TOOLS / "donutboot.py"),
                            "unpack", str(STOCK_BOOT)],
                           capture_output=True, text=True, cwd=work)
        self.assertEqual(r.returncode, 0, r.stderr)
        argv = ["pack", "--kernel", str(work / "Image")]
        for i in range(5):
            argv += ["--dtb", str(work / f"dtb{i}.dtb")]
        argv += ["--ramdisk", str(work / "ramdisk.gz"),
                 "--cmdline", self.cmdline,
                 "-o", str(work / "repacked.img")]
        run(*argv)
        got = self.parse(work / "repacked.img")
        want = self.h
        for field in ("ka", "ra", "sa", "ta", "ps", "cmdline"):
            self.assertEqual(got[field], want[field], field)
        self.assertEqual(got["ramdisk"], want["ramdisk"])
        _, got_payload = self.split_mtk(got["kernel"])
        _, want_payload = self.split_mtk(want["kernel"])
        self.assertEqual(self.split_payload(got_payload)[0],
                         self.split_payload(want_payload)[0],
                         "decompressed kernel identical")
        self.assertEqual(self.split_payload(got_payload)[1],
                         self.split_payload(want_payload)[1],
                         "all five DTBs identical, same order")


if __name__ == "__main__":
    unittest.main(verbosity=2)
