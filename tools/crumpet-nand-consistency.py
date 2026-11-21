#!/usr/bin/env python3
"""Caps-consistency lock for the crumpet mt8167 NFC/ECC driver overlay.

Parses the machine-readable caps-block from reference/crumpet-nfi-register-map.md
(the Task 1 contract, §4, between the `<!-- caps-block` and `-->` markers) and
validates the crumpet-linux overlay driver sources and binding yamls against it:

  crumpet-linux/drivers/mtd/nand/raw/mtk_nand.c   mtk_nfc_caps_mt8167 + of_match
  crumpet-linux/drivers/mtd/nand/ecc-mtk.c        mtk_ecc_caps_mt8167 + of_match
  crumpet-linux/Documentation/devicetree/bindings/mtd/*.yaml  compatible enums

Exits nonzero on any mismatch or missing mt8167 entry (plan Task 5, TDD: run
before the overlay files exist and watch it fail; then implement and go green).
Stdlib only.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "reference" / "crumpet-nfi-register-map.md"
NFC_C = ROOT / "crumpet-linux" / "drivers" / "mtd" / "nand" / "raw" / "mtk_nand.c"
ECC_C = ROOT / "crumpet-linux" / "drivers" / "mtd" / "nand" / "ecc-mtk.c"
NFC_YAML = (ROOT / "crumpet-linux" / "Documentation" / "devicetree" /
            "bindings" / "mtd" / "mediatek,mtk-nfc.yaml")
ECC_YAML = (ROOT / "crumpet-linux" / "Documentation" / "devicetree" /
            "bindings" / "mtd" / "mediatek,nand-ecc-engine.yaml")

REQUIRED_KEYS = [
    "nfc.compatible", "nfc.spare_size", "nfc.num_spare_size",
    "nfc.pageformat_spare_shift", "nfc.nfi_clk_div", "nfc.max_sector",
    "nfc.max_sector_size",
    "ecc.compatible", "ecc.err_mask", "ecc.err_shift", "ecc.ecc_strength",
    "ecc.num_ecc_strength", "ecc.ecc_mode_shift", "ecc.parity_bits",
    "ecc.pg_irq_sel", "ecc.ecc_regs",
]

# mt2712 ECC register layout the caps-block name must resolve to, in enum
# order ECC_ENCPAR00, ECC_ENCIRQ_EN, ECC_ENCIRQ_STA, ECC_DECDONE,
# ECC_DECIRQ_EN, ECC_DECIRQ_STA — register map §3 + T2 disasm (DECFSM/DECIRQ
# @0x124/0x200, ENCPAR bank 0x300) and §4a (mt8167 41-entry table contains
# 0x300..0x31C, 0x80, 0x124).
MT2712_ECC_REGS = [0x300, 0x80, 0x84, 0x124, 0x200, 0x204]

errors = []


def err(msg):
    errors.append(msg)
    print(f"FAIL: {msg}")


def ok(msg):
    print(f"ok: {msg}")


def genmask(expr):
    m = re.fullmatch(r"GENMASK\(\s*(\d+)\s*,\s*(\d+)\s*\)", expr)
    if not m:
        return None
    hi, lo = int(m.group(1)), int(m.group(2))
    return ((1 << (hi - lo + 1)) - 1) << lo


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def parse_caps_block():
    text = DOC.read_text()
    m = re.search(r"<!--\s*caps-block\s*\n(.*?)-->", text, re.S)
    if not m:
        err(f"caps-block markers not found in {DOC.relative_to(ROOT)}")
        return {}
    caps = {}
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep:
            err(f"caps-block line is not 'key = value': {line!r}")
            continue
        caps[key.strip()] = value.strip()
    for key in REQUIRED_KEYS:
        if key not in caps:
            err(f"caps-block is missing key {key}")
    return caps


def struct_field(block, name):
    m = re.search(r"\." + name + r"\s*=\s*([^,}]+)", block)
    return m.group(1).strip() if m else None


def c_int(text):
    return int(text, 0)


def int_list(text):
    return [int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", text)]


def resolve_array(src, name):
    m = re.search(r"(?:static\s+)?(?:const\s+)?(?:u8|u16|u32|int)\s+" + name +
                  r"\s*\[\s*\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        return None
    return m.group(1)


def check_file(path, what):
    if not path.is_file():
        err(f"{what} missing: {path.relative_to(ROOT)}")
        return None
    return strip_comments(path.read_text())


def of_match_entries(src):
    return re.findall(r'\.compatible\s*=\s*"([^"]+)"\s*,\s*'
                      r'\.data\s*=\s*&(\w+)', src)


def main():
    caps = parse_caps_block()

    # ---- NFC driver -------------------------------------------------------
    src = check_file(NFC_C, "NFC overlay driver")
    if src:
        m = re.search(r"mtk_nfc_caps_mt8167\s*=\s*\{([^}]*)\}", src)
        if not m:
            err("mtk_nand.c: no mtk_nfc_caps_mt8167 initializer")
        else:
            blk = m.group(1)
            pairs = [
                ("num_spare_size", "nfc.num_spare_size", c_int),
                ("pageformat_spare_shift", "nfc.pageformat_spare_shift", c_int),
                ("nfi_clk_div", "nfc.nfi_clk_div", c_int),
                ("max_sector", "nfc.max_sector", c_int),
                ("max_sector_size", "nfc.max_sector_size", c_int),
            ]
            for field, key, conv in pairs:
                got = struct_field(blk, field)
                want = caps.get(key)
                if got is None:
                    err(f"mtk_nfc_caps_mt8167: field .{field} missing")
                elif conv(got) != conv(want):
                    err(f"mtk_nfc_caps_mt8167.{field} = {got}, "
                        f"caps-block {key} = {want}")
            ref = struct_field(blk, "spare_size")
            want_list = int_list(caps.get("nfc.spare_size", ""))
            body = resolve_array(src, ref) if ref else None
            if body is None:
                err(f"mtk_nand.c: .spare_size = {ref!r} does not resolve "
                    f"to a u8 array initializer")
            else:
                got_list = int_list(body)
                if got_list != want_list:
                    err(f"spare size table {ref} = {got_list}, caps-block "
                        f"nfc.spare_size = {want_list}")
                elif len(got_list) != c_int(caps["nfc.num_spare_size"]):
                    err(f"nfc.num_spare_size = {caps['nfc.num_spare_size']} "
                        f"but table has {len(got_list)} entries")
                else:
                    ok(f"NFC spare table ({len(want_list)} entries), "
                       "shift/clk_div/max_sector/max_sector_size match")

        entries = dict(of_match_entries(src))
        want = caps.get("nfc.compatible")
        if entries.get(want) != "mtk_nfc_caps_mt8167":
            err(f"mtk_nand.c of_match: no entry {want!r} -> "
                f"&mtk_nfc_caps_mt8167 (found: {entries})")
        else:
            ok(f"NFC of_match entry {want!r} -> &mtk_nfc_caps_mt8167")

    # ---- ECC driver -------------------------------------------------------
    src = check_file(ECC_C, "ECC overlay driver")
    if src:
        m = re.search(r"mtk_ecc_caps_mt8167\s*=\s*\{([^}]*)\}", src)
        if not m:
            err("ecc-mtk.c: no mtk_ecc_caps_mt8167 initializer")
        else:
            blk = m.group(1)
            pairs = [
                ("err_shift", "ecc.err_shift", c_int),
                ("num_ecc_strength", "ecc.num_ecc_strength", c_int),
                ("ecc_mode_shift", "ecc.ecc_mode_shift", c_int),
                ("parity_bits", "ecc.parity_bits", c_int),
                ("pg_irq_sel", "ecc.pg_irq_sel", c_int),
            ]
            for field, key, conv in pairs:
                got = struct_field(blk, field)
                want = caps.get(key)
                if got is None:
                    err(f"mtk_ecc_caps_mt8167: field .{field} missing")
                elif conv(got) != conv(want):
                    err(f"mtk_ecc_caps_mt8167.{field} = {got}, "
                        f"caps-block {key} = {want}")

            got = struct_field(blk, "err_mask")
            want_val = genmask(caps.get("ecc.err_mask", ""))
            val = None
            if got:
                val = genmask(got)
                if val is None:
                    md = re.search(r"#define\s+" + re.escape(got) +
                                   r"\s+(.+)", src)
                    val = genmask(md.group(1).strip()) if md else None
            if val is None or val != want_val:
                err(f"mtk_ecc_caps_mt8167.err_mask = {got!r} "
                    f"(= {val}), caps-block ecc.err_mask = "
                    f"{caps.get('ecc.err_mask')} (= {want_val})")
            else:
                ok(f"ECC err_mask {got} == {caps.get('ecc.err_mask')} "
                   f"(0x{val:x})")

            ref = struct_field(blk, "ecc_strength")
            want_list = int_list(caps.get("ecc.ecc_strength", ""))
            body = resolve_array(src, ref) if ref else None
            if body is None:
                err(f"ecc-mtk.c: .ecc_strength = {ref!r} does not resolve "
                    f"to a u8 array initializer")
            else:
                got_list = int_list(body)
                if got_list != want_list:
                    err(f"strength table {ref} = {got_list}, caps-block "
                        f"ecc.ecc_strength = {want_list}")
                elif len(got_list) != c_int(caps["ecc.num_ecc_strength"]):
                    err(f"ecc.num_ecc_strength = "
                        f"{caps['ecc.num_ecc_strength']} but table has "
                        f"{len(got_list)} entries")
                else:
                    ok(f"ECC strength ladder ({len(want_list)} entries), "
                       "err_shift/mode_shift/parity_bits/pg_irq_sel match")

            ref = struct_field(blk, "ecc_regs")
            if ref != caps.get("ecc.ecc_regs"):
                err(f"mtk_ecc_caps_mt8167.ecc_regs = {ref!r}, caps-block "
                    f"ecc.ecc_regs = {caps.get('ecc.ecc_regs')!r}")
            else:
                body = resolve_array(src, ref)
                regs = {}
                if body:
                    for name, val in re.findall(r"\[(\w+)\]\s*=\s*(0x[0-9a-fA-F]+)",
                                                body):
                        regs[name] = int(val, 16)
                got_regs = [regs.get(f"ECC_{n}") for n in
                            ("ENCPAR00", "ENCIRQ_EN", "ENCIRQ_STA",
                             "DECDONE", "DECIRQ_EN", "DECIRQ_STA")]
                if None in got_regs or got_regs != MT2712_ECC_REGS:
                    err(f"ecc_regs table {ref} = {got_regs}, expected the "
                        f"mt2712 layout {MT2712_ECC_REGS}")
                else:
                    ok(f"ECC regs table {ref} == mt2712 layout "
                       f"{[hex(v) for v in MT2712_ECC_REGS]}")

        entries = dict(of_match_entries(src))
        want = caps.get("ecc.compatible")
        if entries.get(want) != "mtk_ecc_caps_mt8167":
            err(f"ecc-mtk.c of_match: no entry {want!r} -> "
                f"&mtk_ecc_caps_mt8167 (found: {entries})")
        else:
            ok(f"ECC of_match entry {want!r} -> &mtk_ecc_caps_mt8167")

    # ---- binding yamls ----------------------------------------------------
    yaml = check_file(NFC_YAML, "NFC binding yaml")
    if yaml:
        if f"- {caps.get('nfc.compatible')}" not in yaml:
            err(f"{NFC_YAML.name}: compatible enum lacks "
                f"{caps.get('nfc.compatible')}")
        elif f"const: {caps.get('nfc.compatible')}" not in yaml:
            err(f"{NFC_YAML.name}: no per-compatible if-block for "
                f"{caps.get('nfc.compatible')}")
        else:
            ok(f"NFC binding yaml: enum + if-block for "
               f"{caps.get('nfc.compatible')}")
    yaml = check_file(ECC_YAML, "ECC binding yaml")
    if yaml:
        if f"- {caps.get('ecc.compatible')}" not in yaml:
            err(f"{ECC_YAML.name}: compatible enum lacks "
                f"{caps.get('ecc.compatible')}")
        else:
            ok(f"ECC binding yaml: enum entry for "
               f"{caps.get('ecc.compatible')}")

    if errors:
        print(f"\ncrumpet-nand-consistency: {len(errors)} failure(s)")
        return 1
    print("\ncrumpet-nand-consistency: all caps checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
