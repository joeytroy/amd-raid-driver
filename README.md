# ⚡ rcraid (Multi-Format TUI-Enhanced Edition)

[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](https://opensource.org/license/gpl-2-0)
![Kernel: Linux 7.0+](https://img.shields.io/badge/Kernel-Linux%207.0%2B-orange.svg)
![DKMS: out-of-tree](https://img.shields.io/badge/DKMS-out--of--tree-informational.svg)

Your motherboard's AMD RAIDXpert2 NVMe storage arrays become a native, standard block device node right at **/dev/rcraid0**. Partition it, format it, configure it dynamically from a clean terminal wizard, and **boot Linux straight from it with zero graphical overhead.**

---

## 🚦 Deployment Status — Baseline Release (2026-10-02)

> **⚠️ DEPLOYMENT WARNING — READ BEFORE INSTALLING.**
> The hardware-validated baseline is **RAID 0 and RAID 1**. **RAID 10 is
> feature-complete** — reads/writes/discards, per-pair degraded failover, member
> rebuild, and boot-time degraded assembly are all implemented — **but its only array-box run so far used metadata written by this driver**
> (an `rcadm -C -r10` 2×2 array, 2026-11-13); a firmware-created array has not
> been assembled yet, so treat it as unproven for production data until
> [`docs/VALIDATION_RAID10.md`](docs/VALIDATION_RAID10.md) is signed off.
> **RAID 5/6** assemble **read-only** by default: the driver identifies them
> and reads them. RAID 5 has an opt-in write path
> (`enable_writes=1 parity_writes=1`): full rows generate P directly, partial
> rows read-modify-write the row under one volume-wide write lock. That path
> was **hardware-validated 2026-11-13** on a 4-member array stamped by this
> driver — full-row and partial-row writes, with the parity chunk XOR-checked
> on media. Still unvalidated: power-loss/write-hole behaviour, parity rebuild,
> a firmware-created parity array, and reading an array the vendor wrote.
> RAID 6 has no write path at all.
> Not deployable yet.
> The mirror completion and array rebuild paths run on a
> **modern non-blocking asynchronous thread runner** (`kamd_async_worker` parks
> parent-bio endio off the hardware completion context; `kamd_resync` owns
> long-running rebuilds), so hardware completion contexts never block. All
> **legacy mechanical spinning-platter (SATA HDD) models are completely
> dropped** — this branch services the AMD NVMe RAID controller (`1022:B000`)
> only. Do not deploy it against a spinning-disk array.

*   **Kernel target:** Linux **7.0+**, consumed as an **out-of-tree DKMS**
    module built against the running kernel's headers — not an in-tree driver.
*   **Verified build:** the tree compiles **cleanly, with zero warnings and zero
    errors**, against `7.0.0-34-generic` headers (a second clean build was
    verified against `7.0.0-38-generic`).
*   **RAID 10 (Nested Striped Mirror):** the nested striped-mirror layout takes a real blk-mq
    dispatch path — writes and discards mirror to **both** copies of each
    2-way pair, reads round-robin across the pair, and losing one copy per
    pair degrades the array instead of failing it. A stale or replaced member
    is rebuilt **from its own mirror partner** (the only copy that holds the
    same column's stripes), one column at a time, with a logical
    write-exclusion window so application writes can't race the copy. The
    array **assembles degraded at boot** as long as every pair keeps at least
    one copy, then rebuilds the missing copy when it returns; a pair that has
    lost both copies refuses assembly rather than exposing an unreadable
    volume.
*   **RAID 10 geometry fix (2026-10-02):** the `rc_amd_map_nested_raid10` translation plane now correctly splits addresses across flash geometries with no single-stripe bottlenecks. Per-column physical LBA calculation uses `div_u64` for 64-bit safety; column assignment via modulo arithmetic is branch-predictor friendly. Legacy block boundaries restricting enterprise flash throughput have been removed.
*   **Real-firmware corroboration (2026-10-07):** the metadata format and the RAID 10 geometry model were validated against four raw member dumps from a firmware-created X399 array (George "PlaidPiper" Crossley, [`PlaidPiper/x399-raidcore-fixtures`](https://github.com/PlaidPiper/x399-raidcore-fixtures), reported in [joeytroy#57](https://github.com/joeytroy/amd-raid-driver/issues/57) and cross-linked from [issue #1](https://github.com/kubuntugamer/amd-raid-driver/issues/1)). All four dumps parse clean through this driver's validation chain, and a byte-level cross-dump of the members confirms the mirror-pair convention is **adjacent** (pairs `{0,1}` / `{2,3}`) — the last unverified RAID 10 assumption. Details in [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md).
*   **Fault isolation hardened:** all transient device fault-isolation loops now cleanly catch and handle `BLK_STS_RESOURCE` boundaries, guaranteeing total array resilience against unexpected hardware dropouts. The async completion engine returns `BLK_STS_RESOURCE` on allocation failure or unload race, never `BLK_STS_IOERR`, so the block layer retries instead of reporting false I/O errors to the filesystem.
*   **RAID 5/6: recognised and readable; RAID 5 writes opt-in (2026-11-08, writes 2026-11-13).** The level is taken from the LD `DeviceType` alone (`0x1BF5` RAID5, `0x1BFA` RAID6) — `FirstCount × SecondCount` cannot express a parity layout, so a count-encoded `0x1BF6` record is never mistaken for one, and a RAID 6 record can no longer be assembled as RAID 10. The parity encoding is now **confirmed against a BIOS-created 3-disk RAID 5** (capacity `2 ×` per-member user size, `DeviceType 0x1BF5`); an earlier revision had the two values swapped from the dispatch-table inference. A parity volume assembles, reports the right capacity (`(N-1)` / `(N-2)` × per-member user size) and serves reads through the shared map in `src/patch_parity_math.c`; writes and discards are refused by default, and RAID 5 writes are opt-in and were hardware-validated 2026-11-13. The parity **placement** is settled from the vendor core — **dedicated parity on member `N-1`, no rotation** (issue #6) — and the RAID 6 **Q encoding** is settled too: **EVENODD/RDP diagonal parity with prime 17 and no GF arithmetic at all** (issue #3) — and the generator now implements exactly that (`src/raid6_evenodd.h`, issue #10), with the Galois code gone; it still has no caller while the volume stays read-only. The layout itself rests on AMD's code rather than on inference from one array, so what remains is *corroboration* from a vendor-written array — not an open question about the map — plus parity rebuild, write-hole handling, firmware acceptance of a driver-stamped array, and reading a vendor-written one.
*   **Enterprise roadmap (this driver, not experimental):**
    *   **OCE (Online Capacity Expansion)** — add drives to RAID 5/6 arrays online, restripe parity, grow filesystems without downtime.
    *   **ORLM (Online RAID Level Migration)** — migrate RAID 0↔5↔6↔10 online, zero host CPU, instant metadata commit.
    *   These are **first-class roadmap items for this driver** — not experimental, not imported. Enterprise management ops are first-class citizens.
*   **Frozen scope:** remaining experimental engine code paths (FabricZC, HighPoint metadata import) are **intentionally frozen** for **post-migration execution phases**. They remain in-tree but are experimental; do not rely on them for production data. **Prime directive: nothing from experimental migrates to main.**

---

## 🗺️ Enterprise Roadmap (This Driver — Not Experimental)

Phases 0–8 — including **OCE**, **ORLM**, COW snapshots, P2PDMA, io_uring and
ZNS/FDP offload — are tracked in **[`docs/IMPLEMENTATION.MD`](docs/IMPLEMENTATION.MD)**
under "Long-horizon phases", with one status per phase. That file is the
single source of truth for both the long view and the near-term order; the
table is deliberately not duplicated here, so the two can't drift apart.

Where the phases actually stand: **Phase 0** (RAID 0/1/10 + rebuild) is done
for RAID 0/1 and awaiting the RAID 10 array-box run; **Phase 1** (parity)
assembles and reads, and RAID 5 writes are opt-in and hardware-validated; **Phase 2** (metadata writer + TUI
create) is the next PR — the TUI can select 10/5/6 today, but the metadata
writer only produces 0/1, so those choices stop at an error instead of
stamping a layout the driver can't validate; phases 3–8 are long-horizon.

**Prime directive: nothing from experimental branch migrates to main. OCE/ORLM/P2PDMA/io_uring are first-class features of *this* driver.**

---

## 🎛️ RAID Format State Matrix

The baseline release is deliberately narrow. RAID 0 and RAID 1 are the
production-supported, hardware-validated rows; RAID 10 is feature-complete and
next in line for its array-box run; RAID 5/6 assemble and read, and refuse
every write by default. RAID 5 writes (full rows and read-modify-write of
partial rows) exist under `enable_writes=1 parity_writes=1` and were
hardware-validated 2026-11-13 on a 4-member array stamped by this driver;
discards and all RAID 6 writes remain refused. Power-cut behaviour, parity
rebuild and a firmware-created parity array are still unvalidated.

| Format | State | Notes |
| --- | --- | --- |
| **RAID 0 (Stripe)** | ✅ **Baseline — stable** | Full horizontal data chunk striping. |
| **RAID 1 (Mirror)** | ✅ **Baseline — stable** | Duplicated writes across pairs; round-robin load-balanced reads; degraded failover + non-blocking resync. |
| **RAID 10 (Nested)** | 🟡 **Feature-complete — validation pending** | Striped row segments over 2-way mirrored pairs (≥ 4 drives). Writes/discards mirror to both copies, reads round-robin, per-pair degraded failover. Pair-aware member rebuild/resync and boot-time degraded assembly landed; a lost pair refuses assembly. Mirror-pair convention (adjacent `{2c, 2c+1}`) confirmed against real X399 firmware dumps (2026-10-07), but the NVMe `1022:B000` data path has run **only with metadata this driver wrote** (`rcadm -C -r10`, 2×2, 8 TB, 2026-11-13); a firmware-created array has not been assembled yet, and the [runbook's](docs/VALIDATION_RAID10.md) results table is still empty. |
| RAID 5 (Parity) | 🟡 **Writes opt-in; degraded/rebuild validated 2026-11-13** | Single-parity, dense per-row packing. Level comes from the LD `DeviceType` only (`0x1BF5`, confirmed on a BIOS-created 3-disk array 2026-11-08). Volume assembles at `(N-1)` × per-member capacity and serves reads; writes/discards are refused by default. Without the opt-in the queue is capped at one stripe per request, because the parity-chunk address is the *same* row slot as the data chunk it protects; with it, full rows are fanned out and partial rows are read-modify-written, serialized by one write lock. **Level/geometry hardware-confirmed.** Parity placement is settled from the vendor blob — **dedicated parity on member `N-1`, no rotation** — and the map (`rc_amd_map_distributed_raid5`) now implements it; the earlier row-rotating map disagreed on two rows in three and was replaced 2026-11-08 ([VALIDATION_RAID5.md](docs/VALIDATION_RAID5.md), issue #6). **Layout settled against the vendor implementation**; not yet corroborated by reading a vendor-written array, and member order comes from the LD element array. The volume is read-only by default; the opt-in `parity_writes=1` path was hardware-validated 2026-11-13 on a driver-stamped 4-member array (full rows and read-modify-write, parity checked on media), discards are refused, degraded reads reconstruct a failed member from parity, and `START_RESYNC` rebuilds it from the survivors (both hardware-validated 2026-11-13); there is still no write-hole journal or automatic rebuild. DELETE handles parity LDs, and the clean-room `rxp2adm` can create a RAID 5 array behind `parity_create=1` — metadata only over erased members, with no data/parity initialisation. |
| RAID 6 (Dual Parity) | 🟡 **Read-only — assembly + reads wired** | P + Q dual parity (`0x1BFA`); data on `0..N-3` with P/Q dedicated to `N-2`/`N-1`, matching the vendor blob (the P/Q order within those two is assumed). Same read-only contract as RAID 5, capacity `(N-2)` × per-member. The vendor's Q is **EVENODD/RDP diagonal parity (prime 17, XOR plus an integer modulus, no Galois field)**, settled from `rc_raid6_evenodd.cpp` (issue #3) — and the generator in this tree implements that construction (`src/raid6_evenodd.h`, issue #10), with no Galois code at all. It still has no callers. No reconstruct/verify path exists, so nothing reads parity yet; a healthy array is read from its data members alone. |

---

## 🚀 Key Production Features

*   **Interactive Phase 1 Terminal GUI (TUI):** No pre-configured arrays are required on boot. On clean, blank testing drives, the live-installer automatically spins up a keyboard-driven blue menu canvas (`dialog`). Testers can check off raw NVMe paths using the Spacebar, then choose a target level from a list the **built driver itself reports** (`0 1 10 5`) and which is filtered by the member count just picked — RAID 1 wants exactly two, RAID 10 an even four or more, RAID 5 at least three, and RAID 6 is always shown as `NOT SUPPORTED` because there is no Q write/parity path (**RAID 5 is read-only until the parity-write opt-in is set**, and RAID 10 is feature-complete but awaiting a firmware-array run) — and commit layouts on the fly. Layout metadata is written by `scripts/qemu-test/mkmeta.py`, which covers **RAID 0, 1 and 10** (and RAID 5 behind `parity_create=1`): the menu is filtered to what the built driver reports it can create, and RAID 6 stops with an explicit message instead of stamping a layout the driver cannot validate.
*   **Defensive Hardware Security Guards:** The TUI contains an automated validation length check that enforces a hard **maximum 8-disk constraint** matching AMD firmware specifications, safely preventing memory overrun kernel oops before touching the PCIe bus registers.
*   **Advanced Rootfs Probing:** Phase 2 avoids fragile, hardcoded partition name assumptions. It uses a read-only sandboxed mount-and-probe matrix to peak inside array partitions, locate the genuine system `/etc/os-release`, and seamlessly execute chroot boot configurations on any distribution setup.
*   **Non-Blocking Asynchronous Completion Engine:** Parent-bio completion is parked on the `kamd_async_worker` kthread queue, so hardware completion contexts return immediately and never stall the block-layer queue; a separate `kamd_resync` thread owns long-running rebuilds. Over-reports from faulty member paths are absorbed by a bounded tombstone pool instead of being fatal.
*   **Persistent Boot Locks:** Automated **DKMS (Dynamic Kernel Module Support)** registration ensures the module automatically re-compiles against any incoming system kernel updates, paired with initramfs hooks and fallback `efibootmgr` NVRAM UEFI string injection.

---

## 🅐 Quick Start: Fresh OS Installation (From Live USB)

The recommended path for a fresh Ubuntu / Kubuntu / Mint / Fedora install that
lives **on** the AMD RAIDXpert2 NVMe array. Three steps: the script stands the
array up, you partition it from the desktop installer, then the script finishes
the target system.

Firmware Secure Boot must be **off** for the live session — the live kernel
refuses the unsigned module, and the key it would need cannot be enrolled
without rebooting that session. Signing and enrolling the key is what lets you
switch Secure Boot back **on** afterwards (Step 4).

### Step 1 — Provision the array and expose `/dev/rcraid0`

1. Boot the live USB, pick **Try Kubuntu** (the live session), and open
   Konsole.
2. Clone this fork and run the entry script:
   ```bash
   git clone https://github.com/kubuntugamer/amd-raid-driver.git
   cd amd-raid-driver
   sudo ./install-livecd.sh
   ```
   It installs its build dependencies, compiles `rcraid.ko` against the running
   kernel, and builds the `rxp2adm` writer.
3. It first looks for an array **RAIDXpert2 already created**: RAIDCore
   metadata at LBA `0x5000`. When it is there, exactly those disks become the
   members and nothing is written to them — level and geometry come from the
   firmware's own records, RAID 10/5/6 included.
4. Only when no disk carries that metadata does the **blue Terminal GUI** open,
   because blank disks carry no layout to read:
   - **Spacebar** checks the member disks (hard limit 8, matching firmware),
     then `< OK >`.
   - Then the level. The list is the driver's own: it prints
     `Driver reports it can create: 0 1 10 5` above a menu filtered by the
     member count you just picked — RAID 1 wants exactly two, RAID 10 an even
     four or more, RAID 5 at least three. RAID 6 always appears as
     `NOT SUPPORTED: driver has no Q write/parity path` and cannot be chosen.
     The clean-room writer (`scripts/qemu-test/mkmeta.py`) stamps **RAID 0, 1
     and 10**, and RAID 5 only behind `parity_create=1`; anything the driver
     cannot validate stops with an explicit message instead of being written.
     That path writes **fresh metadata over the selected disks**, so it is for
     empty drives only.
5. Either way the script unbinds the members from `nvme`, binds `rcbottom`,
   forces a probe on each PCIe port, and stands up **`/dev/rcraid0`**, then
   reports the volume and whether it is **writable for the level it read**:
   RAID 0/1 install fine; **RAID 5/6 come up read-only by default** (RAID 5
   writes need the explicit parity-write opt-in) and the script stops there
   rather than handing a read-only disk to an installer.
6. It ends with a prompt and then waits:
   ```
   ➡️  Target /dev/rcraid0 under manual partitioning, map root, and let files copy.
   Press [Enter] ONLY when the graphical OS installer finishes copying file sectors...
   ```

Four 3.6 TB members, for reference, come up as a 16.00 TB RAID 0 that the
installer sees as **`rcraid0 - 14.55 TiB (/dev/rcraid0)`**.

### Step 2 — Partition and install from the desktop installer

Minimise the terminal. **Do not close it** — Step 3 continues in that window.
Launch the distribution's installer (on Kubuntu: *Install Kubuntu*) and work
through to **Partitions**:

1. **Select storage device → `rcraid0 - 14.55 TiB (/dev/rcraid0)`**, then
   choose **Manual partitioning**. ("Erase disk" is not the path here — the
   array usually sits beside the live USB, and manual is what guarantees the
   ESP lands on the array.)
2. **New Partition Table → GUID Partition Table (GPT)**, confirm.
3. Create the **ESP**: *Size* **512 MiB**, *File System* **fat32**,
   *Mount Point* **`/boot/efi`**, flag **`boot`** checked. The ESP has to be a
   partition of `rcraid0` itself: shim and grub land there, and that is what
   lets the box boot with every other disk pulled. Nothing kernel-sized lives
   on it (kernels and initramfs stay in `/boot` on the root), so 512 MiB is
   what Ubuntu sizes to and 1 GiB is only worth it for a Windows dual-boot or
   heavy firmware-capsule use.
4. Create the **root**: *Size* whatever remains, *File System* **xfs**,
   *Mount Point* **`/`**.
5. **Location / Keyboard / Users** as you like, then **Install**, and wait for
   **“All done.”** Leave *Restart now* **unticked** — the live session still has
   work to do, and rebooting is your call, not the installer's.

### Step 3 — Finish the target system from the terminal

1. Back in the waiting terminal, press **Enter**. The script mounts the target
   root and bind-mounts the virtual filesystems, then runs inside the new
   system (`==> [3/3] Mapping Mount Bridges and Diving Inside the Target OS...`).
2. Inside the target it installs DKMS and the kernel headers, builds and
   installs `rcraid.ko` for **every** installed kernel, bakes the initramfs
   hook in, persists the driver options (including any parity-write opt-in),
   and rebuilds the initramfs.
3. It then writes the **firmware boot entry** for the array's own ESP and
   verifies it, printing:
   ```
   • firmware boot entry: 'rcraid Array Boot' -> /dev/rcraid0p1
   ```
   Failure here is loud and stops the run — a silent `|| true` is what once
   left a box with no boot entry at all. The entry is appended to the existing
   boot order rather than forced to the front; put it first in firmware setup,
   or with `sudo efibootmgr -o <XXXX>,...` using the number it printed.
4. If you accept the module-signing key offer —
   `Enrol the installed system's module-signing key in MOK (needed to re-enable Secure Boot later)? [Y/n]`
   — you are asked for the MOK password **yourself**: type it once (echoed, so
   you can see what you typed), minimum 8 characters, the same floor the MOK
   manager enforces. It is never generated, never printed afterwards, and
   never written to a state file. Unattended runs take it from
   `RCRAID_MOK_PASSWORD`.
5. The run ends with `🎉 DONE! The fresh bare-metal PC is 100% prepped. Reboot
   whenever you choose to launch your array.` The script never reboots for you.

### Step 4 — First boot, then (optionally) Secure Boot

The array boots as soon as the install finishes, with Secure Boot still **off**.
Signing only matters if you want to switch Secure Boot back **on**; if you never
do, skip this step and leave firmware Secure Boot off.

**If you accepted the signing key in Step 3** (the default), the key is already
on the target and its enrolment is queued — go to point 1 below.

**If you skipped it** (answered no, or set `RCRAID_MOK_ENROLL=0`), boot the
installed system with Secure Boot still off, then create, use and queue the key
in one step:

```bash
cd ~
git clone https://github.com/kubuntugamer/amd-raid-driver.git
cd amd-raid-driver
sudo RCRAID_MOK_ENROLL=1 ./install-dkms.sh
```

Re-running the installer is safe and is the supported way to do this: it reuses
an existing key or creates one, points DKMS at it so kernel-update rebuilds stay
signed, rebuilds `rcraid.ko` signed, and queues the enrolment.

1. From the checkout, run `sudo ./scripts/verify-boot-safety.sh`. The
   pending-enrolment notice it prints is expected here; a missing or unsigned
   module is not.
2. Reboot. At shim's blue **MOK management** screen:
   **Enroll MOK → Continue → Yes**, type the password **you** chose, and let it
   reboot — MOK manager reboots by itself when it is done, as part of the same
   boot, not an extra step. It runs **before** the kernel, which is exactly why
   the module inside the initramfs can already be signed with a key that boot
   trusts.
3. Enable **Secure Boot** in firmware and reboot once more.
4. Confirm the driver survived:
   ```bash
   mokutil --sb-state                                    # SecureBoot enabled
   modinfo -F signer "/lib/modules/$(uname -r)/updates/dkms/rcraid.ko"
   lsblk /dev/rcraid0                                    # volume + partitions
   ```

Dropping the MOK prompt leaves the key untrusted: with Secure Boot on, the
kernel rejects `rcraid.ko` and the array is missing at that boot — which cannot
complete at all when `/` lives on the array. The screen returns on the next boot
until it is answered, and `sudo mokutil --import /var/lib/shim-signed/mok/MOK.der`
re-queues a request that was dropped. This whole flow needs the boot chain to go
through shim (the default on Ubuntu, Debian and Fedora); a kernel started
directly by firmware through its own EFI stub never runs MOK manager, so there
Secure Boot has to stay off.

---

## ⚖️ License & Cleanroom Verification
This project is licensed under the **GPL-2.0-only** standard. It is a 100% clean-room, independently authored driver framework reverse-engineered from publicly distributed vendor binaries under DMCA §1201(f) interoperability protections—**no proprietary source code or confidential documentation was utilized.**
