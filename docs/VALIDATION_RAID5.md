# RAID 5 Hardware Validation Runbook

This is the checklist for the **read-only-by-default** RAID 5 path. There is
no destructive write phase to run: the driver refuses every write and discard
unless loaded with `enable_writes=1 parity_writes=1`. With that opt-in, full
rows generate P directly and partial rows are read-modify-written under one
volume-wide write lock. There is no write-hole journal; the write path is
QEMU-verified and was hardware-validated on a 4-member driver-stamped array on
2026-11-13 (see below), and degraded reads/parity rebuild are hardware-validated
(see the second run section). The parity **placement** is no
longer the open question — it is settled against the vendor core (§3) and the
driver's map already follows it. What remains is corroboration on
vendor-written bytes and power-cut behaviour.

## What is already confirmed on hardware

A RAIDXpert2 BIOS-created **3-disk RAID 5** (3× Sabrent Rocket Q 3.7 TB,
`1022:B000`) produced this committed `RC_LogicalDevice` record. That array
was removed on 2026-11-09, so the record below is the surviving evidence and
the runbook has no current fixture to run against — re-running it requires
creating a new array in the RAIDXpert2 UEFI utility first:

| Field | Observed value |
|---|---|
| `DeviceType` | `0x1BF5` |
| `Devices` | `3` |
| `FirstCount` / `SecondCount` | `1` / `3` |
| `Capacity` | `15,625,838,592` sectors |
| per-member `UserDataSize` | `7,812,919,296` sectors |
| per-member `UserDataOffset` | `0x105000` sectors (1,069,547,520 B) |
| `ChunkSize` / `ChunkIndex` | `0` / `3` → **256 KiB** stripe |

Capacity is exactly `(N-1) × UserDataSize`, which is RAID 5. This also
settles the DeviceType encoding: `0x1BF5` is **RAID 5**, `0x1BFA` is
**RAID 6**. The vendor dispatch agrees — it derives the parity count as
`(DeviceType == 0x1BFA) + 1`. The driver's constants were swapped before
2026-11-08 and are now corrected.

The layout itself is now read out of the vendor core — dedicated parity on
the last member, dense packing, 256 KiB chunks (`docs/REVERSE_ENGINEERING.md`
item 7) — and the RAID 6 Q encoding is settled as EVENODD with prime 17
(item 8). What is still **not** confirmed is that layout against data written
by the vendor firmware/driver. A freshly created array is zeroed, so
XOR checks on it are vacuous — all-zero data and all-zero parity agree with
every candidate layout.

## Hardware write run — 2026-11-13 (PASS)

4× Sabrent Rocket Q 4 TB behind `1022:B000`, kernel `7.3.0-10-generic`, root on
a USB disk, Secure Boot off. The members carried a 4-member RAID 5 committed
generation in this tree's writer layout: `DeviceType 0x1BF5`, `FirstCount` 1,
`SecondCount` 4, chunk index 3 (256 KiB), capacity `23,438,757,888` sectors,
`UserDataOffset 0x105000`. It assembled `optimal` with
`enable_writes=1 parity_writes=1 parity_create=1`.

- **Writes.** Two complete rows (768 KiB each, one O_DIRECT write per row) and
  four partial writes: 4 KiB inside a data chunk, 8 KiB straddling a chunk
  boundary, 8 KiB straddling a row boundary, and 4 KiB in the last data chunk.
  After `fsync` and a cache drop, the 1.5 MiB span read back byte-identical
  (`sha256 cf7e8780…c476e4`).
- **On-media parity.** Driver unloaded, members returned to `nvme`, raw 256 KiB
  chunks read at `0x105000 + row × 256 KiB`. Members 0–2 matched the expected
  logical bytes, and member 3 equalled their XOR for both rows
  (`c454ba9291f7d53f…`, `0fd6774f8b40930e…`).
- **Persistence.** A full `rmmod` and reload re-assembled the same 4-member
  RAID 5 (`optimal`, same capacity) and the span read back identically.
- **Default gate.** Reloading with `enable_writes=1` but without
  `parity_writes=1` gave `ro=1`, and a 4 KiB write was refused.
- **Metadata untouched.** The 4 MiB window at LBA `0x5000` on each member was
  byte-identical to the pre-run backup after the writes.
- **Discard.** `blkdiscard` on the read-write parity volume exited non-zero.

Not covered by this run: power cut / write hole, firmware or BIOS acceptance of
a driver-stamped parity generation, a vendor-written array, and any other
member count or chunk size.

## Degraded read + parity rebuild — 2026-11-13 (PASS)

Same 4× Sabrent box, kernel `7.3.0-10-generic`, on a freshly created 256 MiB
RAID 5 (`rxp2adm create -l 5 -d 0 1 2 3 -s 256`) loaded with
`enable_writes=1 parity_writes=1 parity_create=1`.

- **Healthy write.** 64 MiB of random data written with O_DIRECT at LBA 0 and
  read back byte-identical.
- **Degraded read.** Member 1 was failed through
  `echo 1 > /sys/block/rcraid0/rcraid/fail_member`. The volume reported
  `degraded`; the same 64 MiB read back byte-identical with every chunk on the
  missing member reconstructed from P plus the two surviving data members. A
  write while degraded was refused with `EIO`.
- **Rebuild.** `AMD_IOCTL_START_RESYNC` rebuilt member 1 over 174,762 sectors
  (85 MiB, the whole per-member user area, including the final 170-sector
  partial-page tail). The rebuild completed, the state returned to `optimal`,
  and the 64 MiB readback plus an 8 MiB write at offset 64 MiB both passed.
- **Repeat.** A separate fresh-create 9 MiB run passed the same sequence
  (healthy, degraded, rebuild, optimal).

Two fixes came out of this run: parity volumes allocate the per-member coherent
RMW buffers even when writes are disabled, because degraded reads and rebuild
need them; and the synchronous PRP path now expresses a transfer whose last
page is partial, so the rebuild tail completes.

Limits: one failed member only (two are fatal and the volume reports `failed`),
writes are refused while degraded, rebuild is manual via START_RESYNC, state is
in-memory only (a reboot reassembles as in-sync), and only 4 members × 256 KiB
chunks were tested. The write hole and parity initialisation remain open.

## Fast path — one command

`scripts/validate-raid5.sh` runs the non-destructive checks on the array box:

```sh
sudo ./scripts/validate-raid5.sh              # sampled reads, read-only checks
sudo ./scripts/validate-raid5.sh --full-read  # also read the whole volume
```

It never writes to the array. It does verify that a write is **refused**.
Paste the whole output for sign-off.

## Preconditions

- [ ] At least 3 NVMe members in a **firmware-created RAID 5**.
- [ ] No foreign RAID metadata on the members (a drive from HighPoint/Intel/
      mdadm must be initialized before the AMD array is created).
- [ ] `rcraid` loaded against the three array members. On a box that also has
      a spare/unconfigured `1022:B000` disk, bind **only** the members — a
      disk whose valid RAIDCore metadata does not name it is refused by the
      legacy-fallback guard, but the installer's subsystem-vendor filter will
      still attempt to probe it.
- [ ] Secure Boot: the out-of-tree module must be signed and trusted (MOK) or
      Secure Boot disabled. An unsigned `insmod` fails with
      `Key was rejected by service`.
- [ ] `make KERNELDIR=/lib/modules/$(uname -r)/build` builds clean.

## Operator surface

| What | How | Meaning |
|---|---|---|
| State | `cat /sys/block/rcraid0/rcraid/state` | RAID5 is `optimal`, `degraded` or `resyncing`; one failed member stays readable, writes are refused until rebuilt |
| Members | `cat /sys/block/rcraid0/rcraid/members` | one line per LD position |
| Detail | `/sys/kernel/debug/rcraid/volume` | level, member count, per-member state |
| Read-only | `blockdev --getro /dev/rcraid0` | `1` without the write opt-in, `0` with it |
| Load record | `dmesg \| grep rc_volume_parse_logical_device` | raw DeviceType/counts/capacity/chunk |

Without `parity_writes=1` (and a full-row-capable geometry) the volume stays
read-only; `enable_writes=1` alone is not enough.

**`fail_member` is supported on RAID 5 for one member.** The volume reports
`degraded`, reconstructs reads from parity and refuses writes; a second failed
member is fatal. Trigger a rebuild with `AMD_IOCTL_START_RESYNC` once the
member is usable again.

## 0. Trust gate

- [x] `dmesg | grep -i UNTRUSTED` contains **only** the expected
      `RAID 5/6 is assembled READ-ONLY …` reason. Any capacity-mismatch or
      unknown-chunk line is a bug — capture it. (2026-11-08: only the
      read-only reason present.)

## 1. Level and geometry

- [x] `debugfs volume` reports `level: raid5` and `members: N`.
- [x] The parse line shows `devtype=0x1bf5`, `devices=N`, and
      `capacity = UserDataSize × (N-1)`.
- [x] `blockdev --getsize64 /dev/rcraid0 / 512` equals that capacity.
- [x] The chunk resolves to a known stripe (index 1/2/3 → 64/128/256 KiB).

All four above were checked on the 3-disk array, 2026-11-08: `level=raid5`,
`members=3`, `devtype=0x1bf5`, `devices=3`, capacity `15,625,838,592`
sectors, `chunk_idx=3` → 256 KiB.

## 2. Read path

- [x] `scripts/validate-raid5.sh` sampled reads pass at 0/25/50/75/100 % and
      are stable across a cache drop.
- [x] A write to `/dev/rcraid0` fails and the disk stays read-only.
- [x] No new `rcraid` I/O error / timeout / reset lines in dmesg.

All three were checked on 2026-11-08: 18 checks PASS. Note that the script's
read-only-veto check could not pass before `6ced208` — `dmesg | grep -q` under
`set -o pipefail` returns 141 from SIGPIPE, so it always reported "veto line
not found". See the procedural note at the end of §3b.

## 3. Layout confirmation — answered from the vendor binary (issue #6)

**The vendor placement is dedicated parity on the last member, not the
rotating map this tree used to implement.** Read out of the DWARF-bearing
`rcblob.x86_64` (SDK 9.3.0-00283); the derivation is in
`docs/REVERSE_ENGINEERING.md` ("RAID 5/6 layout"):

| | vendor blob | this driver (since 2026-11-08) |
|---|---|---|
| RAID 5 data member | `(lba >> chunk_pow) % (N-1)` → members `0..N-2` | same |
| RAID 5 parity member | **`N-1`, every row** | `N-1`, every row |
| RAID 6 | data `0..N-3`, P/Q on `N-2`/`N-1` | same (P/Q order assumed) |
| row stride | `(N - parity_count) × chunk`, packed per row | same |

`patch_parity_math.c` used to rotate (`parity_pos = (N-1) - (row % N)`), which
disagreed with the vendor on two rows in three for `N=3`; it now implements the
dedicated layout. The change showed up on the dev array immediately: reloading
the rebuilt module still assembled `optimal` and reads stayed stable, but the
first 64 MiB hashed `7321098d…ed2a3` instead of the `63bf7f7d…c83673` the
fixture had been written under, because that fixture sat on the members in the
old rotating layout. Re-stamping it under the dedicated layout — 128 rows,
member 0/1 alternating, member 2 the row XOR — brought the read back to
`63bf7f7d…c83673`: the same logical bytes at a new physical placement, which is
what §3b records. Do not read that round trip as corroboration of the vendor's
write side; it only shows the driver's read path agrees with its own map.

The layout is therefore **settled against the vendor implementation** — the
arithmetic is AMD's own and no read, parity-lock or parity-memory path applies
a row term — rather than inferred from one array. What is still missing is
*corroboration on real bytes*: the staged fixture is written by this driver
(re-stamped under the dedicated layout and round-tripping cleanly), so it
cannot discriminate between candidate layouts. Two things stay unobserved
rather than unsettled: no vendor-written array has been read back, and member
order comes from the LD element array.

What is still worth doing on hardware, once such data exists, is to confirm
the *read* map end to end:

### 3a. Vendor-written data (non-destructive, preferred)

- [ ] While booted with the AMD RAIDXpert2/Windows driver (or using an array
      that already holds data), write a known pattern — e.g. a file of random
      data, or `mkfs` + files — and record its checksum.
- [ ] Boot Linux with `rcraid`, read the same range through `/dev/rcraid0`
      (or mount read-only), and compare. A clean match across several
      stripe-sized ranges is strong evidence; interleaved garbage means the
      mapping is wrong.
- [ ] Record the observed layout here: `LAYOUT = ______________`.

If no vendor-written array exists and the UEFI utility is not available,
§3c covers producing one with the vendor driver.

### 3b. Direct-member stamp (destructive; validates the driver against whichever map it implements)

This proves the driver reads the map it implements; it cannot prove the vendor
writes that map. Useful for a regression check after the map changes, and to
separate a read-path bug from a layout mismatch.

The **vendor** map for N=3, 256 KiB chunks — what a stamped fixture should
now use:

| row | parity member | data members (stripe 0, stripe 1) | physical offset on every member |
|---|---|---|---|
| 0 | 2 | 0, 1 | `UserDataOffset + 0 × chunk` |
| 1 | 2 | 0, 1 | `UserDataOffset + 1 × chunk` |
| 2 | 2 | 0, 1 | `UserDataOffset + 2 × chunk` |

Parity is fixed on the last member (`N-1`); data chunks are members `0..N-2`
in ascending order, packed at the same row offset on each member. Stamp
`data0`, `data1`, `P = data0 XOR data1` accordingly, load the driver, and
verify `/dev/rcraid0` returns the logical data. Restore zeros afterwards (the
array started zeroed).

- [x] Stamp + read-back matches (2026-11-08): 128 rows / 64 MiB stamped —
      members 0/1 alternating data chunks, member 2 the row XOR — then read
      back byte-identical through `/dev/rcraid0` (`63bf7f7d…c83673`), with the
      ext4 filesystem mounting read-only and all file checksums matching.
- [ ] Restore zeros afterwards — deferred; the array is scratch and the 64 MiB
      region still holds the fixture.

Two procedural traps, both hit on the array box:

1. `rcbottom` pins `driver_override=rcbottom` on every member it owns, so
   binding one back to `nvme` to stamp it raw fails with `EIO` until that
   attribute is cleared (`echo > /sys/bus/pci/devices/<bdf>/driver_override`).
   Restore it before reloading `rcraid`, or the members lose their binding.
2. Do not test "is the module loaded" with `lsmod | grep -q` in a script that
   sets `set -o pipefail`: grep exits on the first match, `lsmod` takes
   SIGPIPE, and the pipeline reports 141 — the test is then always false.
   Read `/sys/module/rcraid` instead. (The same bug silently disabled the
   read-only-veto check in `validate-raid5.sh` until 2026-11-08.)

### 3c. Creating a vendor-written fixture on Linux (vendor-driver bootstrap, 7.3.0-10)

`rcadm -C -r5` refuses before sending a frame: the licence flag comes from
`ControllerInfo`, which this driver deliberately leaves zero rather than
fabricate (`REVERSE_ENGINEERING.md`, "Feature licensing"). With every member
bound to `1022:B000` there are also no `/dev/nvme*` nodes for userspace to
stamp raw. That leaves two ways to obtain firmware-style RAID 5 metadata: the
RAIDXpert2 UEFI utility, or the vendor driver. This subsection is the second.

**Everything here stays outside this repository.** The vendor source, blob and
module are never committed, linked or built by this tree; they exist only to
produce a fixture that the clean-room driver then reads.

Status 2026-11-13: **not yet completed.** The vendor module built and its blob
was patched, but the first load (with the unpatched blob) oopsed inside
`load_module`, which holds `module_mutex`; the lock stayed held, so no module
could be loaded or unloaded afterwards and the patched build has never had a
clean load. Only a reboot clears it.

#### Build fixes for 7.3 (the vendor tree predates it)

- `Makefile`: `EXTRA_CFLAGS` → `ccflags-y`; link the blob with
  `ln -sf rcblob_v2.x86_64 rcblob.x86_64.o`.
- `rc_init.c` PCI ID table: five `.sudiskice = PCI_ANY_ID` → `.subdevice`
  (vendor typo that only surfaces on 7.3).
- `rc_init.c` `bios_param`: `struct block_device *bdev` → `struct gendisk *disk`
  in the prototype **and** the definition (the definition splits the signature
  across lines, so a one-line regex fixes only the prototype and leaves
  conflicting types at the definition).
- Timer API across `rc_init.c`, `rc_msg.c`, `rc_event.c`, `rc_config.c`:
  `from_timer(` → `timer_container_of(`, `del_timer_sync(` →
  `timer_delete_sync(`, `del_timer(` → `timer_delete(`.

#### Blob patches (the part that matters)

`raidxpert2-install` patches the closed v2 blob before build; skipping that is
what produced the crash below. Applied to the pristine `rcblob_v2.x86_64`:

| # | Offset | Change |
|---|---|---|
| 1 — licence level | `0xce4` | `mov eax,0x2a030000; ret` + nops; `endbr64` at `0xce0` preserved |
| 2 — feature gate | `0x6017c` | opcode `0x21` (AND) → `0x39` (CMP) inside `RC_ScanBus` |
| 3 — stack canary | all | 477 × 9-byte `%gs:[0x28]` loads/stores → `xor reg,reg` + nops |

Patch 3 is required because the v2 blob was built with
`-fstack-protector-strong`; on kernels ≥ 6.15 (where `fixed_percpu_data` is
gone) those `%gs:[0x28]` accesses fault. Apply all three to a bytearray read
from the pristine file and assert the length is unchanged — an earlier
in-place attempt left the blob 343 bytes larger and `ld` rejected it with
`section extending past end of file`.

#### First load and the module-mutex wedge

With the unpatched blob the load page-faults at `%gs+0x28` in
`RC_STH::SendRaw()`, called from probe (`rc_send_test` → `rc_msg_init`). That
is inside `load_module`, so the task dies holding `module_mutex`:

- `/proc/modules` shows `rcraid … Loading`; `/sys/module/rcraid/initstate` is
  `coming`; there are no `/dev/rcraid*` nodes.
- Every later `insmod` blocks (timeout, exit 124), `rmmod` reports "in use",
  and `CONFIG_MODULE_FORCE_UNLOAD` is usually unset — there is no in-kernel
  recovery.
- `dmesg` has nothing after the oops, because the next load never began.

Only a reboot clears this. Do **not** retry `insmod` or kill the blocked task;
capture `dmesg | tail -60` first.

#### Post-reboot runbook

0. `lsmod | grep rcraid` — if it is still loaded, reboot again and stop.
1. Load the patched vendor module and wait for init:
   `insmod /var/tmp/rcraid-vendor-patched/src/rcraid.ko`, then poll
   `/sys/module/rcraid/initstate` for up to 60 s until it reads `live`.
   Check `dmesg | tail -30` and that `/dev/rcraid*` and `/dev/rc_api` exist.
   If it is still `coming`, stop (the mutex is already held) and reboot.
2. Confirm the licence passes (`rcadm -M -qa` shows no "not licensed"), then
   `rcadm -C -r5 -d 0 1 2 3`. Expected: exit 0, `ro=0`, capacity
   ≈ 3 × per-element sectors. Record the exact `-M -qa` line and
   `/sys/block/rcraid0/size` — that is the reference the clean-room driver
   has to reproduce.
3. `rmmod rcraid`, load this driver, then verify `level=RAID5`, the capacity
   above, `ro=1`, four members, reads at start/mid/end, and that the metadata
   was not rewritten or validated away. Acceptance is that our parser
   assembles what the vendor stamped; it does not re-prove parity reads.

Afterwards either keep the array as the only real RAID 5 fixture or delete it
with the vendor driver, the UEFI utility, or `rxp2adm delete` under this module —
DELETE now removes parity LDs (`0x1BF5` / `0x1BFA`) as well as `0x1BF6`, and the
QEMU rig exercises delete → create → reassembly. Do not bind devices on a
machine whose OS disk sits behind `1022:B000`.

Fallbacks if the patched blob still wedges: create the array in UEFI
RAIDXpert2 ([`AFTER-REBOOT.md`](AFTER-REBOOT.md)), or boot a kernel in the
vendor's validated 6.15–6.17 range (this driver also builds there). Our writer
can now stamp RAID 5 metadata (`parity_create=1`), but it initialises neither
data nor parity and is not a substitute for a vendor-written fixture.

## 4. Explicitly not testable yet

- Discard remains refused. The write path is now hardware-validated for
  full-row, in-row, chunk-boundary and row-boundary writes, including the
  parity chunk on media, but not across a power cut and not against
  vendor-written data.
- Creating a RAID 5/6 array through AMD's `rcadm`: its licence check stops it
  before any frame is sent (§3c). This driver's own CLI can stamp RAID 5 with
  `parity_create=1`; the box's 4-member array carried a generation in that
  format, and rows 4–6 show such a volume assembling and taking writes on
  hardware. It remains metadata only, with no initialisation of data or
  parity, and firmware/BIOS acceptance of a driver-stamped parity generation is
  untested. RAID 6 CREATE remains refused.
- Member rebuild and degraded reads (RAID5 has no single-member reconstruction
  path in this driver).
- Boot-from-RAID5.
- OCE / ORLM / metadata writer.

## Results

| # | Check | Result | Notes |
|---|---|---|---|
| 0 | Trust gate | PASS 2026-11-08 | only the expected `RAID 5/6 is assembled READ-ONLY` reason |
| 1 | Level / geometry / capacity | PASS 2026-11-08 | `level=raid5`, `members=3`, `devtype=0x1bf5`, capacity 15,625,838,592 = 2 × user size |
| 2 | Read path + read-only enforcement | PASS 2026-11-08 | 18 checks: reads at 0/25/50/75/100 %, cache-drop stable, write refused |
| 3 | Parity placement | settled against the vendor code + map changed | issue #6: dedicated parity on member `N-1`, driver follows it since 2026-11-08; not yet corroborated by a vendor-written read |
| 4 | Write path (full row + RMW) | PASS 2026-11-13 | 2 × 768 KiB full rows + 4 partial writes (in-chunk, chunk boundary, row boundary, last chunk); 1.5 MiB readback byte-identical after cache drop |
| 5 | On-media parity | PASS 2026-11-13 | raw chunks at `0x105000 + row × 256 KiB`: members 0–2 match, member 3 equals their XOR for both rows |
| 6 | Persistence + default gate | PASS 2026-11-13 | rmmod/reload re-assembles `optimal` at the same capacity; without `parity_writes=1` the volume is `ro=1` and refuses writes; metadata window unchanged |
| 7 | Boot / install | blocked | `install-livecd.sh` re-loads a read-only RAID 5 volume with `parity_writes=1` and proceeds only when it reports writable, so an install can run; no boot-from-RAID5 run has been made |
| 8 | Requeue-contract fix under concurrency | PASS 2026-11-14 | 3-member 256 MiB driver-stamped array: 5 rounds × (four concurrent buffered 16 MiB `conv=fsync` writers + one 4 KiB-block writer) — every writer returned 0, all four regions byte-identical after cache drop, no hung-task/parity/error messages; `rmmod` clean and all three members rebound to `nvme` |

Rows 0–2 are the 2026-11-08 evidence from the 3-disk firmware array, which was
removed on 2026-11-09. Rows 4–6 are the 2026-11-13 run on a driver-stamped
4-member array. Row 8 re-ran the write path on the 3-member array after the
2026-11-13 requeue fix, with the concurrency that triggered the wedge. Row 3
still has no vendor-written bytes to check against; row 7 waits on a
boot-from-array run.

Not a hardware result: delete → create → reassembly is QEMU-verified on the
synthetic 4-member fixture (`--create-test`). It does not cover initialising a
real array's data/parity, nor firmware acceptance of the stamped generation.

## Promote

Do **not** mark RAID 5 stable until parity rebuild and write-hole handling
exist. Writes and placement are no longer the gate: the opt-in write path is
hardware-validated (rows 4–6) and the layout is settled against the vendor
code. The README row may say "opt-in writes hardware-validated; no rebuild",
and moves to stable only when rebuild, write-hole handling, and a
firmware-created parity array have run.
