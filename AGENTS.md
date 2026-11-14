# AGENTS.md — working in this repository

This is `rcraid`, a clean-room, out-of-tree Linux kernel block driver for AMD
RAIDXpert2 NVMe arrays (PCI `1022:B000`). It is licensed **GPL-2.0-only**.
These instructions are for automated agents and human contributors alike; the
CI workflows in `.github/workflows/` are authoritative if anything here drifts.

## Hard constraints

- **Clean room.** No code, comments, or control flow may be copied from AMD's
  binaries or the AMD Linux SDK. Only protocol/layout facts derived under DMCA
  §1201(f) are usable. See `docs/RE_METHODOLOGY.md`; if a change needs vendor
  source to explain itself, it does not belong in this tree.
- **Never link or compile vendor material.** `drivers/` is reference input only
  and has been removed from the tree (recoverable from git history, not to be
  restored for building).
- **Every `rc_*.c` / `rc_*.h` must carry `SPDX-License-Identifier: GPL-2.0-only`
  on line 1.** This is a blocking CI gate.
- **Kernel ≥ 6.15.** The driver calls the 2-argument `blk_rq_map_sg()`; older
  kernels (e.g. Debian trixie's 6.12) will not build and are deliberately not in
  the CI matrix.
- **Do not overstate hardware support.** See "Deployment status" below; the
  README's warnings are part of the contract, not documentation decoration.

## Build and verify

```sh
make KERNELDIR=/lib/modules/$(uname -r)/build   # default target; must be warning-clean
make test-async                                  # userspace async-engine harness
make test-async-asan                             # ASan + UBSan + LSan
make test-async-tsan                             # data races
scripts/qemu-test/run-qemu-raid-test.sh --kver "$KVER" --kernel ./vmlinuz --level raid0
```

- Judge warnings only in our sources (`rc_*.c`, `rc_*.h`). `-Wunused-parameter`
  is ignored because kernel callback signatures legitimately carry unused
  parameters; every other warning fails CI.
- `./build.sh` is the convenience wrapper: it cleans, locates kernel headers and
  builds with `-Wno-error`-style leniency. Use plain `make` when checking that a
  change is actually warning-clean.
- Hardware testing on this box is expected and authorised: running `insmod`
  and binding `1022:B000` members by hand is part of the workflow, not a
  forbidden act. The QEMU rig stays the CI gate for the scenarios it covers,
  but it is not a substitute for the array box, and hardware results take
  precedence over rig results.
- Before binding, confirm the OS disk is not one of the `1022:B000` NVMe
  members (`lsblk`, `lspci -d 1022:b000`): that controller class shadows every
  NVMe in the box, so binding it can take the running root away.

## Blocking CI gates (`.github/workflows/ci.yml`)

1. **build** — compiles in Fedora, Arch, Ubuntu 24.04 HWE, and Ubuntu 26.04
   containers; `rcraid.ko` must be produced.
2. **warnings** — no compiler warnings in `rc_*.c` / `rc_*.h` other than
   `-Wunused-parameter`.
3. **qemu-rig** — boots the module against synthetic arrays for `raid0`,
   `raid1`, `raid1-degraded`, `raid1-degraded-boot`, `raid5-parity-write`, and
   `raid5-create-delete`
   (the opt-in RAID5 write path: full-row parity, in-row/boundary/cross-row
   read-modify-write) and `raid5-create-delete` (delete a parity LD, re-create
   it through the driver's writer, require the same capacity/level), then runs
   the guest battery: write/readback, module
   reload cycles, discard+rewrite, `mke2fs` (sub-page buffered writeback),
   and the stale-config-generation decoy.
4. **spdx** — the line-1 license tag above.
5. **whitespace** — `git diff --check` on changed lines; `docs/ghidra_output/**`
   and `drivers/windows/**` are excluded as reference artifacts.
6. **shellcheck** — every tracked `*.sh`, severity warning and up. The install
   and boot scripts can brick a boot.
7. **checkpatch** — **advisory only** (not blocking). Five source files still
   use 4-space indentation; normalize to kernel tabs in the file you touch, but
   don't reformat unrelated code.

Use kernel-driver style: tabs, `/* */` comments, no C99 declarations after
statements unless the surrounding file already does so — match the file.

## Change-quality expectations

The repo's automated review prompt enforces these; write code that passes it:

- blk-mq lifecycle: `blk_mq_start_request()` / `blk_mq_end_request()` exactly
  once per request, including every error path.
- Scatterlist and DMA: every `dma_map_*` has a matching unmap; size SG lists to
  the actual bvec count, not an assumed page-aligned maximum. Fragmented
  buffered writeback (512-byte buffer-head bvecs) has caused real data-path
  bugs here.
- NVMe PRP alignment rules; 64-bit-safe sector/byte arithmetic (`div_u64`,
  no overflow on large arrays).
- Concurrency: document which context (ISR, softirq, process) owns each piece
  of state; no blocking in hardware completion contexts. Parent-bio completion
  is parked on `kamd_async_worker`; long rebuilds belong to `kamd_resync`.
- **Never return a silent error.** Every failure path must unmap, free, end the
  request, and log. Silent `BLK_STS_IOERR` returns have caused debugging rounds
  in this project. Keep `BLK_STS_RESOURCE` for transient allocation/unload
  races so the block layer retries.
- Do not add a feature that writes metadata the driver cannot validate. The
  offline encoder (`mkmeta.py`) emits RAID 0/1 only and stops with an
  explicit message for 10/5/6. The kernel writer emits RAID 0/1/10, and RAID 5
  only behind `parity_create=1` — metadata over members whose user area is
  known empty, never data/parity initialisation. RAID 6 is never written.

## Deployment status (do not contradict these claims)

| Level | State |
|---|---|
| RAID 0 / RAID 1 | Hardware-validated baseline, production-supported |
| RAID 10 | Feature-complete; an `rcadm -C -r10` 2×2 array written by this driver was assembled and read on real hardware 2026-11-13, **but no firmware-created array-box run yet** (`docs/VALIDATION_RAID10.md` runbook exists; its results table is still empty) |
| RAID 5 / RAID 6 | RAID 5: assemble/read, opt-in writes (`enable_writes=1 parity_writes=1`; full rows generate P, partial rows RMW under one lock), and **hardware-validated degraded reads + manual parity rebuild 2026-11-13**. One failed member leaves the volume `degraded` and read-only: reads reconstruct from P plus survivors, writes/discards are refused, and `START_RESYNC` rebuilds the member (64 MiB written, 64 MiB degraded readback, 85 MiB rebuild, optimal readback all passed on a 4-member 256 MiB driver-stamped array). No write-hole journal, no parity initialisation, no automatic rebuild, and state is in-memory only. RAID 6 writes/rebuild refused; read-only by default. DELETE removes parity LDs; RAID 5 CREATE behind `parity_create=1` (metadata only, erased members). Layout settled against the vendor core, not yet corroborated on vendor-written data |

- RAID 5 `DeviceType` is `0x1BF5`, RAID 6 is `0x1BFA`. Level comes from the LD
  `DeviceType` alone — never infer it from member counts.
- Degraded/resync state is in-memory only; the driver never writes firmware
  metadata. Reboot reassembles as if in sync.
- **Prime directive: nothing from the experimental branch migrates to main.**
  OCE, ORLM, P2PDMA and io_uring are first-class roadmap items, not
  experimental imports.

## Where the truth lives

- `docs/IMPLEMENTATION.MD` — single source of truth for roadmap phase status;
  update it instead of duplicating status in the READMEs.
- `docs/STATUS.md` — current state and implementation history.
- `docs/REVERSE_ENGINEERING.md` — protocol/layout findings and open unknowns.
- `docs/RE_METHODOLOGY.md` — legal and clean-room record.
- `.github/workflows/claude-pr-review.yml` — the review standards in full.

Note: the root `README.md` says kernel 7.0+, while `docs/README.md` and CI say
≥ 6.15 (7.0 is the validated target). CI is authoritative.

## Commits and PRs

- Sign off commits (`git commit -s`, DCO). Add yourself to the contributor list
  for non-trivial work.
- Keep PRs scoped; the QEMU rig and the four-kernel build matrix run on every
  PR, so unrelated churn costs CI time.
- Do not add tests that require real RAID hardware to the default path; the
  hardware runbooks (`docs/VALIDATION_*.md`) are manual procedures.
