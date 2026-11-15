# RAID 10 Hardware Validation Runbook

This is the checklist that turns the RAID 10 row from **"Feature-complete —
validation pending"** into **"Baseline — stable."** Nothing here runs on a
development laptop: it is executed on the **array box** (the machine with the
`1022:B000` controller and a firmware-created RAID 10 array). The QEMU rig and
CI exercise the same paths on GitHub's runners, not locally.

Work top to bottom. Steps 1 and 2 are **destructive** — run them against a
scratch array or a scratch partition (`/dev/rcraid0pN`), never against a disk
holding anything you care about.

## Status 2026-11-13 — what has run, and what has not

Two **driver-written** RAID 10 fixtures have exercised the data path on the
array box: `rcstamp --level raid10` over four members (it found the read-copy
bug, and after the fix the write/readback, reload, discard and mke2fs battery
passed) and `rcadm -C -r10` creating a 2×2, 8 TB array through this driver's
writer, which assembled `optimal` and read back 16 MiB. Neither is
firmware-created: the metadata came from this tree.

So **steps 0–1 are still unrun** — nobody has assembled RAID 10 metadata
written by firmware or the vendor driver, and the adjacent pair convention has
not been checked on this box's NVMe path. Steps 2–6 may use a driver-stamped
array, because they exercise the same dispatch, failover, rebuild and boot
paths. The results table below records each row's fixture explicitly: a row
run on driver-written metadata is marked **driver-stamped** and does not
count toward step 1 or toward promotion in step 7.

2026-11-14: the automated pass (`validate-raid10.sh --full`) ran against such a
driver-stamped 2×2 array and is recorded below. The run was stopped part-way
through the rebuild at the operator's request — the module was unloaded, the
members returned to `nvme`, and their metadata windows zeroed — so rows 3–6
are partial or unrun, and no promotion follows from any of it.

## Fast path — one command

Most of steps 0–4 (trust gate, geometry, cache-dropped reads, one-copy
failover, rebuild) is automated by `scripts/validate-raid10.sh`, run **on the
array box**:

```
sudo ./scripts/validate-raid10.sh                          # read-only dry pass
sudo ./scripts/validate-raid10.sh --full --yes             # destructive: writes + rebuild
sudo ./scripts/validate-raid10.sh --full --no-wipe --yes   # rebuild only, data preserved
```

`--no-wipe` skips `test_write_path.sh`, so the array's contents survive (failing
and re-admitting one mirror copy is what RAID is for). Use it when you cannot
erase the array; quiesce it first. It still reads across the whole device,
exercises degraded failover, and drives the rebuild write path — it only omits
the standalone write torture. The rebuild wait auto-sizes from the device
(≈3.5 h for 4 TB at a 300 MB/s floor); override with `--timeout N`. Paste the
full output for sign-off. The fatal whole-pair boundary, the rebuild-sourcing
proof, and steps 5–6 stay manual.

## Read-only smoke test on a RAID 0 array

While the box still carries a firmware RAID 0, `scripts/hw-test-array.sh`
proves the read-only path end to end without mounting or writing anything:

```
sudo ./scripts/hw-test-array.sh --dry-run   # preconditions and metadata parse
sudo ./scripts/hw-test-array.sh             # binds read-only and compares stripes
```

It takes capacity, chunk size and element order from the committed LD record
and reads reference chunks from every member **before** unbinding `nvme`. It
then binds the members to `rcbottom` with writes disabled and checks that
logical stripe `i` equals member `i`'s bytes at its `UserDataOffset` for two
stripe rounds, reads the last 4 MiB, and proves a write is refused. A trap
restores the controllers on every exit path, then re-verifies both checksummed
metadata blocks with `scripts/qemu-test/verify-checksums.py`.

The script models **RAID 0 only** (one chunk per member per stripe round) and
refuses a record whose `FirstCount × SecondCount` is not `members × 1`. A
RAID 10 array interleaves logical stripes across mirror columns, so use
`validate-raid10.sh` there instead.

## Preconditions

- [ ] 4 (or any even number of) NVMe members in a **firmware-created RAID 10**.
- [ ] Members carry **no foreign RAID metadata** — drives previously used in a
      HighPoint / Intel / mdadm array must be initialized (RAIDXpert2
      "Initialize" or secure erase) before the AMD array is created. AMD cannot
      import foreign metadata, so going from another controller to AMD is a
      fresh, destructive create.
- [ ] OS running from a **non-array** drive (or a live USB) for the
      destructive phases.
- [ ] `make KERNELDIR=/lib/modules/$(uname -r)/build` builds clean, zero
      warnings.
- [ ] `fio` installed (`apt-get install fio` / `dnf install fio`).

## Operator surface

| What | How | Meaning |
|---|---|---|
| State | `cat /sys/block/rcraid0/rcraid/state` | `unassembled` / `failed` / `resyncing` / `degraded` / `optimal` |
| Members | `cat /sys/block/rcraid0/rcraid/members` | `N <bdf> live\|failed\|needs-resync\|resyncing`, one per slot |
| Rebuild | `cat /sys/block/rcraid0/rcraid/resync_progress` | `none`, or `memberN cur/tot pct%` |
| Fail a copy | `echo <slot> \| sudo tee /sys/block/rcraid0/rcraid/fail_member` | latch member `slot` failed (0-based) |
| Re-admit | `echo 1 \| sudo tee /sys/bus/pci/devices/<bdf>/rcraid/reset` | reset controller; member returns `needs-resync` |
| Detail | `/sys/kernel/debug/rcraid/volume` | geometry, per-member state, cursor |

`allow_degraded` is a load-time module parameter (`0444`): set it via
`modprobe rcraid allow_degraded=1` or a `modprobe.d` drop-in — it cannot be
changed at runtime.

## 0. Trust gate

- [ ] `dmesg | grep -i UNTRUSTED` is **empty**. If not, the parser vetoed the
      geometry — stop, capture the dmesg line, and treat it as a bug.

## 1. Geometry and the pair convention — do this FIRST

**Status: the pair convention is now settled.** PlaidPiper's 2026-10-07
cross-dump of an X399 SATA RAID10 (all four members read at the same
`UserDataOffset`; element 0 ≡ element 1, element 2 ≡ element 3, pairs
differ) confirms the driver's **adjacent** assumption (`{2c, 2c+1}`) on
real hardware — see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)
"External corroboration: X399". Note what is reproducible: the fixtures
are metadata-only (LBA `0x5000`–`0x6000`, and they stop short of
`UserDataOffset`), so the metadata checks can be re-run from the repo,
while the pair-identity compare cannot — it rests on the reported `dd`
output.

**Scope of that confirmation: 2×2 only.** The corroborated set is
`FirstCount=2, SecondCount=2`. `rc_ld_level_from()` admits any
`FirstCount >= 2` with `SecondCount == 2` as RAID10 (`SecondCount > 2` is
rejected), and `rc_volume_phys_for_member` then pairs `{0,1}`, `{2,3}`,
`{4,5}`, … — so a 6-member 3×2 array would be assembled on the same
**unverified** adjacent assumption. No 3×2 (or 2×3) firmware creation has
been cross-dumped; the same 1 MiB per-member compare below is what would
settle it. Until then, treat a non-2×2 RAID10 as read-only/scratch. What that experiment did not cover is
this box's NVMe path, so keep the falsification below as a cheap sanity
check before trusting writes: a wrong pairing does not crash — it
mirrors to the wrong partner and reads across non-partners, i.e. silent
corruption.

- [ ] Confirm geometry: `members` count is even, `cols = members/2`, and the
      debugfs `volume` line agrees with the firmware (`FirstCount × SecondCount`).
- [ ] Falsify the pairing, either:
  - **(a) pre-existing data** — if the array already holds firmware-written
    filesystem data, read a checksummed range through `/dev/rcraid0`. A wrong
    pair map returns interleaved garbage, so a clean read is strong evidence; or
  - **(b) raw compare** — from a state where the members are **not** bound to
    `rcbottom` (live USB before load, or Windows), read a small region past each
    member's `UserDataOffset` and record which two match byte-for-byte.
- [ ] Record the observed pairing here: `PAIRING = ______________`.
      Adjacent expected → proceed. Strided → **STOP**; only the
      copy→member mapping in `rc_volume_phys_for_member` changes (`{c, c+cols}`),
      fix, rebuild, re-run from step 0.
      (Strided would contradict the X399 cross-dump — if that is what you see,
      it means SATA and NVMe firmware use different conventions; re-check
      element order vs. port order before changing anything.)

## 2. Data path

- [ ] `sudo ./test_write_path.sh --yes /dev/rcraid0` (destructive) passes all
      four phases: fragmented buffered writes + `fsync`, stripe-unaligned
      direct writes, `mkfs.ext4` + `e2fsck -fn`, and a clean dmesg tripwire.
- [ ] `fsync`, FUA and `fstrim` behave (no "rejected" / "failed SC/SCT" lines).
- [ ] Reads round-robin across each pair; writes/discards reach **both** copies.

## 3. Degraded failover (per column)

- [ ] Fail one copy of one pair:
      `echo 0 | sudo tee /sys/block/rcraid0/rcraid/fail_member`
- [ ] `state` → `degraded`; I/O keeps flowing from the survivor; `members`
      shows slot 0 `failed`.
- [ ] Fatal boundary: fail **both** copies of a pair. `state` → `failed` and
      new I/O fast-fails (the volume is unserviceable, not silently wrong).
- [ ] Re-admit a survivor so the array is serviceable again for step 4.

## 4. Rebuild / resync

- [ ] Reset a failed copy:
      `echo 1 | sudo tee /sys/bus/pci/devices/<bdf>/rcraid/reset`
- [ ] It returns `needs-resync`; the engine rebuilds it **from its pair
      partner** (the only member holding the same column's stripes).
- [ ] Watch `resync_progress` advance while running **concurrent writes** to
      `/dev/rcraid0`.
- [ ] When it reaches `optimal`, prove the sourcing: fail the *partner* and read
      back data written during the rebuild — it must be present and correct.

## 5. Boot-degraded assembly

- [ ] With `allow_degraded=1`, make one copy of one pair absent, reboot: the
      array assembles `degraded` and rebuilds the missing copy when it returns.
- [ ] Make **both** copies of a pair absent, reboot: assembly **refuses** (no
      `/dev/rcraid0`) rather than exposing an unreadable volume.

## 6. Install and reboot

- [ ] Install Kubuntu onto `/dev/rcraid0`, reboot.
- [ ] `scripts/verify-boot-safety.sh` (module + initramfs present for every
      kernel), then `bench.sh` and `test_driver.sh`.

## 7. Promote — only after every box above is checked

- [ ] `README.md`: RAID 10 row → `✅ **Baseline — stable**`; drop the
      "pending real-hardware validation" wording from the warning and bullet.
- [ ] `docs/STATUS.md`: dated `## <date> — RAID10 hardware validation` entry;
      move RAID10 out of "Not yet".
- [ ] CI: add `raid10`, `raid10-degraded`, `raid10-degraded-boot` to the
      `qemu-rig` matrix.

## Run environment (2026-11-14)

Array box: 4 × Sabrent Rocket Q 3.6 TB behind `1022:B000`, kernel
`7.3.0-9-generic`, root on `/dev/sda2` (non-array), 2×2 RAID 10 stamped by
this tree (`rcstamp --level raid10`, 256 KiB chunks), `enable_writes=1`.

One incident worth recording, because it affects how step 6 is run: this
**installed** system also carries the live-ISO packages — `casper`,
`calamares`, `calamares-settings-kubuntu` and `kubuntu-installer-prompt` — and
its SDDM state preselected `/usr/share/wayland-sessions/kubuntu-live-environment.desktop`.
Every login therefore started `/usr/libexec/start-kubuntu-live-env`, which runs
`kwin_wayland … kubuntu-installer-prompt` (the "Try or Install Kubuntu"
dialog) and then `startplasma-wayland`; `XDG_SESSION_DESKTOP` is forced to
`KDE`, so the session looks like Plasma. The prompt was silenced by pointing
the remembered session at `plasma.desktop`; the packages remain installed.

That makes Calamares resident in the session used for these steps, but it is
**not** the supported live-ISO session, and no installer run has yet been made
against `/dev/rcraid0` (see row 6).

## Results

All rows below were executed against **driver-stamped** metadata, not a
firmware-created array.

| # | Check | Result | Notes |
|---|---|---|---|
| 0 | Trust gate | PASS (driver-stamped) | 4 members, `rcstamp --level raid10`, 256 KiB chunk; `RC_MetaData`/`RC_ConfigCommit` checksums re-verified in place on all four members (`verify-checksums.py`: 4/4 MATCH); **no** `UNTRUSTED` line; volume `optimal`, `live_mask 0xf`, capacity 15,625,935,872 sectors (8 TB). |
| 1 | Pair convention | NOT RUN on this box | The adjacent convention rests on PlaidPiper's X399 cross-dump (2026-10-07) only. No firmware-created RAID 10 exists on this box, so the raw per-member compare could not be made. |
| 2 | Write path | PASS | `test_write_path.sh` on the full volume: fragmented 512 B–4 K buffered writes + cache-dropped on-media CRC verify, stripe-unaligned 1 MiB direct writes + verify, `mkfs.ext4` + `e2fsck -fn` clean, dmesg tripwire clean (no `rejected` / `failed SC/SCT` / I/O error lines). |
| 3 | Degraded + fatal boundary | PARTIAL | Fail one copy (`fail_member 0`) → `state=degraded`, `members` showed slot 0 failed, reads still served by the survivor. Fatal whole-pair boundary and the fast-fail check were not reached. |
| 4 | Rebuild + concurrent writes | PARTIAL | Reset slot 0 → `needs-resync`; resync advanced to 43% of 7,812,967,936 sectors at ≈1.4 GB/s while a 64 MiB `O_DIRECT` write at logical 2 GiB completed during the rebuild. Completion to `optimal` and the partner-fail sourcing proof were **not** reached — the run was stopped, the module unloaded, member state discarded. |
| 5 | Boot-degraded + whole-pair refusal | NOT RUN | Requires a reboot with a member (then both copies of a pair) absent. |
| 6 | Install / reboot / boot-safety | NOT RUN | Calamares is resident in the session (see "Run environment") but was not run against this RAID 10 `/dev/rcraid0`; the live-session install onto `rcraid0` on 2026-11-14 was a RAID 0 array (README quick start). `VALIDATION_OSINSTALL.md` is explicitly payload-only, not a Calamares run. `verify-boot-safety.sh` / `bench.sh` / `test_driver.sh` not run in this pass. |
