# Restore runbook — donut unit (M0 draft, rehearse before any flash)

Status: **draft written at M0, nothing flashed yet.** Every command below is
staged for the M0 rehearsal; none has been executed against the unit. The
unit (192.168.10.155, root adb) is the **production WHA oracle** and the only
donut we have — every write needs a manifest-verified backup and a readback
step, and the rehearsal section at the bottom must be fully green first.

Companion docs: `docs/m0-checklist.md` (M0 execution order), `tools/m0/`
(harvest/audit kit). Design basis: `recovery-notes/donut-mainline-design.md`
§5.4; partition wisdom: techo5 `docs/porting-plan.md` + `docs/hardware-donut.md`.

## Ground rules

1. **Never write without a verified backup.** The artifact must hash against
   the harvest `manifest.txt` (`python3 tools/m0/manifest.py --check
   <harvest-dir>`) *and* the partition being replaced must be re-read and
   hashed immediately before the write.
2. **Write to resolved `/dev/block/mmcblk0pN` nodes, never through by-name
   links.** amonet's TWRP plants decoy by-name symlinks (`/tmp/ota-decoy`);
   the `<name>_real` link and `harvest/evidence/decoy-map.txt` carry the
   truth. Resolve at write time: `readlink -f /dev/block/platform/*/by-name/<name>_real`.
3. **Verify every write by readback** — hash the partition after writing and
   compare against the artifact hash (see "Readback verification").
4. **One partition per operation.** No batch scripts on the device.
5. The preloader is **not** a GPT by-name partition: it lives on the
   `mmcblk0boot0` hardware partition.

## Partition map (techo5-observed donut GPT; the unit's live map is evidence)

The authoritative map is the unit's own by-name listing, captured by M0 at
`<harvest>/evidence/by-name-report.txt` (+ `partitions.tsv` with resolved
nodes and sizes). Vendor expectation (techo5 `docs/hardware-donut.md`):
`kb p1, dkb p2, lk_a p3, tee1 p4, lk_b p5, tee2 p6, expdb p7, misc p8,
persist p9, boot_a p10, boot_b p11, swdl p12 (32 MB), system_a p13,
system_b p14, cache p15, userdata p16 (1.5 GB)`; preloader on
`mmcblk0boot0`; no `recovery` partition. If M0's listing disagrees, the
listing wins and this runbook gets updated before any rehearsal.

## Artifact → partition → write method → verify

| Harvested artifact | Target (resolved node) | Method 1: rooted dd | Method 2: kamakiri kit | Method 3: fastboot | Verify |
|---|---|---|---|---|---|
| `partitions/preloader.img` (+ `mmcblk0boot0.img`) | `/dev/block/mmcblk0boot0` | dd | kit BROM step (writes preloader directly) | — | readback hash |
| `partitions/mmcblk0boot1.img` (harvest pull is best-effort) | `/dev/block/mmcblk0boot1` | dd (only if the harvested copy was non-blank) | — | — | readback hash |
| `partitions/lk_a.img` | `lk_a` node (p3) | dd | kit fastboot | `fastboot flash lk_a` | readback + audit marker check |
| `partitions/lk_b.img` | `lk_b` node (p5) | dd | kit fastboot | `fastboot flash lk_b` | readback + audit marker check |
| `partitions/boot_a.img` | `boot_a` node (p10) | dd | kit fastboot | `fastboot flash boot_a` | readback hash |
| `partitions/boot_b.img` | `boot_b` node (p11) | dd | kit fastboot | `fastboot flash boot_b` | readback hash |
| `partitions/swdl.img` | `swdl` node (p12) | dd | kit fastboot | `fastboot flash swdl` | readback hash |
| `partitions/tee1.img` / `tee2.img` | `tee1`/`tee2` nodes (p4/p6) | dd | kit fastboot | `fastboot flash tee1/tee2` | readback hash |
| `partitions/expdb.img` | `expdb` node (p7) | dd | kit fastboot | `fastboot flash expdb` | readback hash |
| `partitions/misc.img` | `misc` node (p8) | dd | kit fastboot | `fastboot flash misc` | readback hash; see misc note |
| `partitions/persist.img` | `persist` node (p9) | dd | kit fastboot | `fastboot flash persist` | readback hash; IDME lives here — never casual |
| `partitions/kb.img` / `dkb.img` | `kb`/`dkb` nodes (p1/p2) | dd | kit fastboot | — | readback hash |
| `partitions/userdata.img` (1.5 GB, provisioned Fire OS state) | `userdata` node (p16) | dd (slow) | kit fastboot | `fastboot flash userdata` (slow) | readback hash (1.5 GB — budget time) |
| `firmware/system-vendor-firmware/…`, `wifi-firmware/*.cfg/.bin/.dat` | files under `/system/vendor/firmware` on the running slot | `adb push` + permissions fix | n/a (Fire OS must be up) | n/a | pull back + `sha256sum` vs manifest |
| IDME (`evidence/idme.txt`) | `persist`-backed idme store | **no tooling — do not write** | — | — | — |
| (reference only) `ota-donut/2019/ota_out/boot.img` | boot slots — **factory fallback only** | dd | kit fastboot | fastboot | readback hash |

The `system_a`/`system_b` partitions are **not** harvested (not boot-chain;
the full 1 GB images live in `ota-donut/2019/ota_out/system.img` as the
factory reference). `cache` is disposable and not harvested.

### Method 1 — rooted-unit dd (primary when M0 shows a patched LK)

```bash
SERIAL=192.168.10.155:5555
H=m0-harvest-YYYYmmdd-HHMMSS          # the verified harvest dir
NODE=$(adb -s "$SERIAL" shell "readlink -f /dev/block/platform/*/by-name/lk_a_real" | tr -d '\r')
[ -n "$NODE" ] || { echo "node resolution failed for lk_a_real"; exit 1; }

adb -s "$SERIAL" push "$H/partitions/lk_a.img" /data/local/tmp/m0-restore.img
adb -s "$SERIAL" shell "dd if=/data/local/tmp/m0-restore.img of=$NODE bs=1048576 conv=fsync"
adb -s "$SERIAL" shell "rm /data/local/tmp/m0-restore.img"
# then readback verification (below)
```

### Method 2 — kamakiri-donut kit (flash fallback when adb/LK is unusable)

Kit checkout: `/home/am/Work/SRE-Device/tools/kamakiri` (docs:
`recovery-notes/community-soc-linux-survey.md` §5). BROM entry needs
**physical access**: unit powered off, **action (dot) button held while
power is applied**, USB wired to the **TM4/TM5 test pads** (D−/D+; ground
TM1/TM3) — feasibility of this route is exactly what audit.sh's manual
fields record. After `bootrom-step.sh` the unit sits in fastboot; flash by
GPT name (`fastboot flash lk_a …`), preloader via the kit's boot0 path; for
GPT damage there is `gpt-fix.sh`. This route bypasses LK entirely — it is
the way back when Method 1 is impossible (e.g. LK overwritten with a
non-booting payload).

### Method 3 — fastboot direct (when the patched LK is alive)

If audit.sh captured a `getvar` sweep, the patched LK exposes fastboot after
`adb reboot bootloader` (or button entry) on the TM4/TM5 USB. Then:

```bash
fastboot -s SERIAL flash lk_a "$H/partitions/lk_a.img"
fastboot -s SERIAL getvar partition-size:lk_a    # sanity
```

## Readback verification (binary-safe)

`adb shell` mangles binary (LF→CRLF); `adb exec-out` does not. Readbacks go
through exec-out:

```bash
# whole-partition hash vs the artifact hash in manifest.txt:
adb -s "$SERIAL" exec-out "dd if=$NODE bs=1048576 2>/dev/null" | tr -d '\r' > /tmp/readback.img
sha256sum /tmp/readback.img            # compare to manifest.txt line
# size sanity:
adb -s "$SERIAL" shell "blockdev --getsize64 $NODE"
```

`$NODE` here is the write target from the block above — resolve it per
partition (same `readlink -f …/<name>_real`, same `[ -n "$NODE" ]` guard)
before each readback; an empty `NODE` must abort, never proceed.

For the 1.5 GB userdata this takes a while — budget it; do not skip it.

## Special cases

- **misc mutates on every boot** (slot tries / boot-control). techo5 keeps a
  timestamped copy beside the old one; do the same: re-harvest misc right
  before relying on a misc restore, and never restore a stale misc onto a
  healthy unit (it can wedge the A/B selector).
- **`lk.img` (stock) is the factory LK** — writing it back re-locks the boot
  chain (removes the patched-LK swdl path; re-unlock then needs the
  kamakiri kit again). It is the last-resort "ship it back to factory boot"
  artifact, not a routine restore source. The unit's own
  `partitions/lk_a.img`/`lk_b.img` are the patched-LK backups to restore.
- **userdata = the WHA-oracle provisioned state.** The full image is taken
  at M0 regardless of the still-open operator question about the restore
  bar (provisioned-state vs factory-reset). Treat the image as
  irreplaceable: two copies on different disks before any userdata write.
- **IDME** (MAC/serial, `persist`-backed): no write tooling exists in this
  project; restore of `persist` as a whole partition is the only path.

## Stock reference inventory (factory fallback set, `ota-donut/2019/ota_out/`)

| File | sha256 |
|---|---|
| `boot.img` (NS6529, 2019) | `a36fa3c583d94a294d8069243a7ac22def0311ad1cc406fe39df92408490321c` |
| `lk.img` (stock/factory LK) | `f670913e136ec8319ed170ca28be6d3cc01727ca367b282c06ebd0f386940a78` |
| `preloader.img` | `f02a67b5aa2bfb0888a57593bfeb4c8c90ff28bbdfdc5d29d56019ad9e8081bb` |
| `system.img` (1 GiB) | `7069ebc3e798772dda69dc70df2f4d7724334b8355a5dee7a55509f34bbf346f` |
| `tee1.img` | `d52ffd7e030898cb87a83db8ca572bd14b23e84a0c7e957edf5806499c975a14` |
| `tee2.img` | `d52ffd7e030898cb87a83db8ca572bd14b23e84a0c7e957edf5806499c975a14` |

## Rehearse-before-first-flash checklist (M0 done-when: "restore path written + rehearsed, no flash yet")

Rehearsal means **everything short of writing**: every step below is
read-only on the unit.

1. [ ] `python3 tools/m0/manifest.py --check <harvest-dir>` — green (every
       harvested artifact hashes).
2. [ ] Partition map reviewed: `<harvest>/evidence/by-name-report.txt` +
       `partitions.tsv`; write targets written down as resolved
       `/dev/block/mmcblk0pN` nodes; `<harvest>/evidence/decoy-map.txt`
       reviewed and no target resolves through a decoy link.
3. [ ] **Readback drill** (read-only): hash every boot-chain partition from
       the device via the exec-out command above and compare each hash to
       the harvest manifest — proves the verification path works before any
       write relies on it.
4. [ ] Method availability recorded: root adb confirmed (`adb shell id -u`
       = 0), fastboot-on-host present, kamakiri kit checkout +
       venv/payload build verified; primary method per artifact marked in
       the table above.
5. [ ] Restore commands staged per artifact (push → dd → readback), with
       the resolved node numbers filled in; second operator (or a re-read
       after a break) reviewed that no target is a decoy link.
6. [ ] Stock reference inventory present and hashed (table above); 1.5 GB
       `userdata.img` exists in **two** independent copies.
7. [ ] USB-pad/BROM-entry feasibility recorded (audit manual fields:
       TM4/TM5 wiring, action-button entry) — decides whether Method 2 is
       a real fallback or wishful thinking.
8. [ ] Sign-off recorded in the M0 report: date, operator, unit serial,
       "no flash performed".
