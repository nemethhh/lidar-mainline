# donut-mainline: mainline Linux and U-Boot for the Amazon Echo Dot 3

Workspace for porting mainline Linux v7.3 and U-Boot v2026.10 to the Amazon
Echo Dot 3rd gen (codename "donut", MediaTek MT8516). It reuses the
[lidar-mainline](../lidar-mainline) overlays (Echo Plus 2nd gen, same SoC) as
the base layer and adds a donut overlay layer on top of them.

## Layout

- `lidar-linux/`, `lidar-uboot/`, `lidar-wifi/`, `lidar.config`, `tools/` —
  verbatim copies from `../lidar-mainline` (the base overlay layer; lidar
  stays the upstream for these, do not diverge).
- `donut-linux/`, `donut-uboot/` — the donut overlay layer (empty for now).
  Applied after the lidar overlays: a file present in both layers ends up
  with the donut version, so donut copies of shared files (e.g.
  `sound/soc/mediatek/Makefile`, dts `Makefile`) must carry lidar's content
  plus donut additions, never a replacement.
- `src/` — pristine linux v7.3 / u-boot v2026.10 checkouts plus build
  directories (`src/build/<O>`); untracked, disposable, recreated by
  `build.sh sync`.
- `out/` — build artifacts (untracked).
- `Containerfile` (with a `Dockerfile` symlink for docker's legacy builder,
  which ignores the Containerfile name) — Debian trixie cross-build container.
- `build.sh` — build pipeline, adapted from lidar's.

## Building

Everything builds inside the container; `src/` and `out/` are bind-mounted,
so the container writes there as root.

    docker build -t donut-build .
    docker run --rm -v "$PWD":/work -v "$PWD/src":/src donut-build bash /work/build.sh fetch
    docker run --rm -v "$PWD":/work -v "$PWD/src":/src donut-build bash /work/build.sh linux lidar Image.gz dtbs
    docker run --rm -v "$PWD":/work -v "$PWD/src":/src donut-build bash /work/build.sh uboot lidar

`bash build.sh` with no target prints the full target list (also in the
script header). `uboot` takes a board argument (default `lidar`); the donut
board arrives with its U-Boot port.

## Overlay stacking self-test

    bash tools/donut-selftest.sh

Asserts with marker files that a donut overlay file shadows the lidar one
and that deleting the donut copy restores lidar's on the next sync. Leaves
the overlays and `src/` pristine.

## Status

Bring-up in progress. The plan: `../../recovery-notes/donut-mainline-m1-plan.md`;
the design doc (constraints, hardware truth): `../../recovery-notes/donut-mainline-design.md`.
