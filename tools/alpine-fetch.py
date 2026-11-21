#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Install Alpine aarch64 packages (with dependencies) into a rootfs directory
without running apk, which cannot execute on the x86 build host.

usage: alpine-fetch.py ROOTFS PKG [PKG...]
The Alpine branch is read from ROOTFS/etc/alpine-release.
"""
import io
import sys
import tarfile
import urllib.request

MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
REPOS = ("main", "community")


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def load_index(branch: str):
    pkgs, provides = {}, {}
    for repo in REPOS:
        base = f"{MIRROR}/{branch}/{repo}/aarch64"
        raw = fetch(f"{base}/APKINDEX.tar.gz")
        with tarfile.open(fileobj=io.BytesIO(raw)) as t:
            text = t.extractfile("APKINDEX").read().decode()
        for block in text.split("\n\n"):
            f = {}
            for line in block.splitlines():
                if len(line) > 2 and line[1] == ":":
                    f.setdefault(line[0], line[2:])
            if "P" not in f:
                continue
            f["url"] = f"{base}/{f['P']}-{f['V']}.apk"
            pkgs[f["P"]] = f
            for p in f.get("p", "").split():
                provides.setdefault(p.split("=")[0], f["P"])
    return pkgs, provides


def resolve(names, pkgs, provides):
    want, todo = [], list(names)
    while todo:
        dep = todo.pop()
        if dep.startswith("!"):
            continue
        dep = dep.split(">")[0].split("<")[0].split("=")[0].split("~")[0]
        name = dep if dep in pkgs else provides.get(dep)
        if name is None:
            raise SystemExit(f"unresolvable dependency: {dep}")
        if name in want:
            continue
        want.append(name)
        todo.extend(pkgs[name].get("D", "").split())
    return want


def main():
    root, names = sys.argv[1], sys.argv[2:]
    ver = open(f"{root}/etc/alpine-release").read().strip()
    branch = "v" + ".".join(ver.split(".")[:2])
    pkgs, provides = load_index(branch)
    for name in resolve(names, pkgs, provides):
        data = fetch(pkgs[name]["url"])
        # an .apk is concatenated gzip streams: signature, control, data
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            members = [m for m in t.getmembers() if not m.name.startswith(".")]
            t.extractall(root, members=members, filter="fully_trusted")
        print("installed", name)


if __name__ == "__main__":
    main()
