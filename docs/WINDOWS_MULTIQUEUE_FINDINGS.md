# Windows rcbottom.sys NVMe multi-queue findings

Investigation of `rcbottom.sys` 9.3.2 to determine how Windows configures
the NVMe controller's queue layout, so we know what to target on Linux.

Source data: `docs/ghidra_output/rcbottom_9.3.2/key_funcs.c`, primarily
`FUN_14000dd44` (NVMe init callback installed at `devExt+0x16110`) and
`FUN_14000f454` (per-queue context setup, called once per queue).

## Confirmed: queue count cap

`FUN_14000dd44` (lines 1944-1960 in `key_funcs.c`) computes the I/O queue
count as:

```c
if (*(char *)(devExt + 0xb4) == '\x01') {
    /* single-queue-forced mode */
}
if (*(int *)(devExt + 0xb0) == 1 || param_2 != '\0') {
    iVar11 = 1;                          /* single queue */
} else {
    iVar11 = *(int *)(devExt + 0xb0) - 1;
    if (4 < iVar11)
        iVar11 = 4;                      /* HARD CAP: 4 I/O queues */
}
*(int *)(devExt + 0x15d1c) = iVar11;     /* stored I/O queue count */
```

**Windows uses at most 4 I/O queue pairs per controller**, regardless of
how many CPUs are available.  `devExt+0xb0` is the MSI vector count
(set elsewhere — not in our current decomp dump, likely populated from a
StorPort callback during adapter init).  Single-queue mode is selectable
via `devExt+0xb4`.

Our Linux driver currently uses **1** I/O queue.  Just matching Windows'
4-queue cap is a 4× concurrency bump on the controller side.

## Confirmed: per-queue depth

`FUN_14000f454` (lines 2310-2326):

```c
uVar4 = (ushort)**(undefined8 **)(param_1 + 0x68);    /* CAP[15:0] = MQES */
psVar1[5] = min(uVar4, 0x100);                         /* SQ depth = 256 */
psVar1[6] = min(uVar4, 0x400);                         /* CQ depth = 1024 */
```

Where `param_1 + 0x68` resolves through the queue-context-pointer table to
`devExt + 0x15940 + 0x68 = devExt + 0x159a8`, which `FUN_14000dd44` line
1943 sets to the MMIO base pointer.  `**(u64**)(MMIO_base)` dereferences
the BAR0 base, reading the first 8 bytes = the NVMe `CAP` register.
`CAP[15:0]` is `MQES` (max queue entries).

On the Crucial T700s in the dev box, `CAP=0x080000203c01ffff` → MQES =
`0xffff`, so depths clamp to:

| Queue | Entries | Bytes |
|---|---|---|
| Per-queue SQ | 256  | 256 × 64 = 16 KiB |
| Per-queue CQ | 1024 | 1024 × 16 = 16 KiB |

Our current SQ/CQ depth is **64** (capped by `RC_NVME_IO_QUEUE_DEPTH`).
Matching Windows = **4× more SQ entries** per queue, **16× more CQ**.

## Confirmed: per-queue memory layout

`FUN_14000dd44` line 1980 allocates `iVar11 * 0x30000 + 0x9fff` bytes
(`192 KiB × N_queues + 40 KiB`).

`FUN_14000f454` lines 2335-2337 zero three regions inside each queue's
192 KiB:

| Offset | Size | Purpose |
|---|---|---|
| 0x00000 | 16 KiB | SQ entries (param_7) |
| 0x08000 | 16 KiB | CQ entries (param_7 + 0x8000) |
| 0x10000 | 128 KiB | PRP list region (param_7 + 0x10000) |
| 0x18000 | 32 KiB | slack |

Plus a static per-controller-per-queue command tracking buffer of
**0xf000 bytes** at `&DAT_14021e8c0 + (queue + (controller_idx - 1) * 4)
* 0xf000` (FUN_14000f454 line 2340-2341).  That's 256 commands × 120
bytes each → exactly matches the SQ depth of 256.

## Implications: total outstanding I/O

| | Linux today | Windows (target) |
|---|---|---|
| I/O queues per controller | 1 | 4 |
| SQ depth per queue | 64 | 256 |
| Total outstanding NVMe commands | **64** | **1024** |

That's a **16× increase** in the maximum number of in-flight commands
the controller can be working on at any moment.  At a 2-member RAID0
that's 2048 outstanding commands total — far more than the dev box
ever sees in any benchmark, but the headroom is what allows full
controller utilization at high QD.

## Not directly confirmed but required by NVMe spec

- **Set Features Number of Queues (admin opc=0x09, FID=0x07)** —
  Windows must issue this before Create I/O CQ/SQ; the NVMe spec
  default is 0 I/O queues unless host explicitly requests more.  Not
  yet located in the decomp (the existing key_funcs.c doesn't include
  the admin command issuer for opc=0x09), but the spec gives us all
  we need to implement it.
- **Per-queue MSI-X vector** — `FUN_14000f608` (the per-queue MMIO
  arming function called from `FUN_14000f454` line 2358) isn't in our
  decomp dump.  Standard practice is one MSI-X vector per CQ, set via
  Create I/O CQ's CDW11.IV field.  Windows almost certainly does this
  since it uses up to 4 queues — same as our plan.

## What we'll implement on Linux

Match the Windows architecture, with one tweak: cap at
`min(num_online_cpus, MSI-X-vectors-available, 4)`.  On the dev box
with hyperthreaded TR 7000 that's 32+ CPUs and the T700 advertises 128
MSI-X vectors — so the cap reduces to 4 (matching Windows).

| Component | Target |
|---|---|
| `RC_NVME_IO_QUEUE_COUNT` | min(num_online_cpus, MSI-X vectors - 1, 4) |
| `RC_NVME_IO_QUEUE_DEPTH` | min(MQES + 1, 256) |
| MSI-X vectors per controller | `RC_NVME_IO_QUEUE_COUNT + 1` (1 for admin) |
| blk-mq `nr_hw_queues` | `RC_NVME_IO_QUEUE_COUNT` |
| Per-CPU queue affinity | via `blk_mq_pci_map_queues` |
| Total outstanding | 4 × 256 = 1024 commands |

Implementation steps (in order):

1. **MSI → MSI-X migration.**  `pci_alloc_irq_vectors` with
   `PCI_IRQ_MSIX | PCI_IRQ_MSI` fallback; allocate `N+1` vectors.
   No behavior change yet (still 1 I/O queue), just validates the path.
2. **Set Features Number of Queues** admin command at init.
3. **Multi I/O queue creation.**  N pairs of SQ/CQ, each with its own
   doorbells, DMA buffers, MSI-X vector wired to its own ISR.
4. **blk-mq per-CPU hctx.**  `nr_hw_queues = N`, `.map_queues =
   blk_mq_pci_map_queues`, each hctx submits on its own SQ and the
   matching CQ's ISR completes its CIDs.
5. **Queue depth bump.**  64 → 256.

## What we'll deliberately differ from Windows

- **No static cmd-tracking buffer** in the driver image.  We use the
  blk-mq tagset for that — gives us 256 tags × N queues × `sizeof(pdu)`
  managed by blk-mq.
- **`scatterlist`-native DMA** (already shipped).  Windows builds PRPs
  from `StorPortGetScatterGatherList` output similarly; the on-the-wire
  layout is the same.
- **Software design follows Linux NVMe driver patterns** (`nvme-core`
  in `drivers/nvme/host/`).  Reverse-engineering Windows for queue
  count was useful as a target, but the actual NVMe protocol exchange
  is all spec-defined.

## Expected impact

Speculative targets based on closing the 1-queue-vs-4-queues gap and
removing the QD=64 cap:

| Workload | Current | Target | Stretch |
|---|---|---|---|
| SEQ1M Q1T1 read | 6.7 GB/s | ~10-12 GB/s | matches Windows ~16 GB/s |
| SEQ1M Q8T1 read aggregate | 11.9 GB/s | ~18-20 GB/s | matches Windows ~19 GB/s |
| RND4K Q32T1 read | (untested) | ~500-700 MB/s | matches Windows ~970 MB/s |

Q1T1 is mostly latency-bound and won't see the full benefit (one
queue is enough for one in-flight request), but per-CPU completion
should drop per-IO latency a bit.  Q8T1 and above is where per-queue
concurrency pays off.
