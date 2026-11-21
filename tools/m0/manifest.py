#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""sha256 manifest for the M0 harvest (pure stdlib, host-side).

Generate (default):
    manifest.py DIR
writes DIR/manifest.txt in sha256sum format - one "<sha256>  <relpath>" line
per file, two spaces, sorted by path - plus DIR/manifest.json, a sidecar
carrying sizes and timestamps per file. The two manifest files themselves
are never listed.

Check:
    manifest.py --check DIR
recomputes every hash in DIR/manifest.txt; exits 1 with a per-file report on
any missing, changed, or unlisted file. docs/restore.md verify steps use this.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import sys

MANIFEST_TXT = "manifest.txt"
MANIFEST_JSON = "manifest.json"
CHUNK = 1 << 20


def iso(t):
    return t.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def iter_files(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn in (MANIFEST_TXT, MANIFEST_JSON):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out.append((rel, full))
    out.sort()
    return out


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_generate(root):
    entries = []
    txt_path = os.path.join(root, MANIFEST_TXT)
    with open(txt_path, "w", encoding="utf-8") as txt:
        for rel, full in iter_files(root):
            st = os.stat(full)
            digest = sha256_of(full)
            txt.write("%s  %s\n" % (digest, rel))
            entries.append({
                "file": rel,
                "sha256": digest,
                "size": st.st_size,
                "mtime_epoch": int(st.st_mtime),
                "mtime_utc": iso(dt.datetime.fromtimestamp(st.st_mtime, dt.timezone.utc)),
            })
    sidecar = {
        "tool": "tools/m0/manifest.py (donut-mainline M0 kit)",
        "generated_utc": iso(dt.datetime.now(dt.timezone.utc)),
        "root": os.path.abspath(root),
        "file_count": len(entries),
        "files": entries,
    }
    with open(os.path.join(root, MANIFEST_JSON), "w", encoding="utf-8") as js:
        json.dump(sidecar, js, indent=1, sort_keys=True)
        js.write("\n")
    print("manifest: %d files hashed -> %s + %s in %s" %
          (len(entries), MANIFEST_TXT, MANIFEST_JSON, root))
    return 0


def cmd_check(root):
    txt_path = os.path.join(root, MANIFEST_TXT)
    if not os.path.isfile(txt_path):
        print("manifest check: FAIL: %s missing" % txt_path, file=sys.stderr)
        return 1
    problems = 0
    listed = set()
    with open(txt_path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("  ", 1)
            if len(parts) != 2:
                print("manifest check: FAIL: line %d: bad format" % lineno,
                      file=sys.stderr)
                problems += 1
                continue
            digest, rel = parts
            listed.add(rel)
            full = os.path.join(root, rel)
            if not os.path.isfile(full):
                print("manifest check: FAIL: missing: %s" % rel, file=sys.stderr)
                problems += 1
            elif sha256_of(full) != digest:
                print("manifest check: FAIL: hash mismatch: %s" % rel, file=sys.stderr)
                problems += 1
    for rel, _full in iter_files(root):
        if rel not in listed:
            print("manifest check: FAIL: unlisted file: %s" % rel, file=sys.stderr)
            problems += 1
    if problems:
        return 1
    print("manifest check: OK (%d files verified)" % len(listed))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="sha256 manifest (txt) + sizes/timestamps sidecar (json) "
                    "for an M0 harvest directory")
    ap.add_argument("--check", action="store_true",
                    help="verify DIR/manifest.txt instead of generating")
    ap.add_argument("dir")
    args = ap.parse_args()
    if not os.path.isdir(args.dir):
        print("manifest: no such directory: %s" % args.dir, file=sys.stderr)
        return 2
    return cmd_check(args.dir) if args.check else cmd_generate(args.dir)


if __name__ == "__main__":
    sys.exit(main())
