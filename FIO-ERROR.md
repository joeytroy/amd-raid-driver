# fio I/O Error on `rcraid0` — Diagnosis

## Symptom

Running a disk benchmark (KDiskMark → fio) against the array-backed root filesystem
(`/dev/rcraid0p2`, ext4) fails almost immediately with:

```
fio: io_u error on file /kdiskmark-jviRoH.tmp: Input/output error: write offset=838860800, buflen=1048576
```

fio is only the messenger here — it is reporting an `EIO` that the `rcraid` driver
returned for a write.

## Root cause (CONFIRMED — fixed)

**A missing hardware-queue dimension on the per-tag PRP-list buffer pool, causing
concurrent commands on different blk-mq hardware queues to corrupt each other's
NVMe PRP list.** This is a host-side driver bug, surfaced (not caused) by the
benchmark's concurrency.

> **Note:** An earlier version of this document blamed a DMA-mapping bug (PRPs built
> from physical instead of DMA addresses, or an unmapped PRP-list buffer). That was
> **wrong** — the driver already uses `sg_dma_address()` off a proper `dma_map_sg()`
> (`rc_volume_build_prp`), and each PRP-list buffer is a `dma_alloc_coherent()`
> allocation on the correct member's device. The real bug is below. The tell was in
> the reproduction table: `dd` O_DIRECT of 1 MiB blocks *also* uses the PRP-list path
> yet passes — so the discriminator was never segment count, it was **concurrency**.

### The bug

The per-tag PRP-list / DSM scratch pool was dimensioned `[member][tag]`:

```c
static __le64     *rc_volume_prp_va[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
static dma_addr_t  rc_volume_prp_pa[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
```

But the volume runs `nr_hw_queues = 4`, and in blk-mq **`req->tag` is unique only
*within* one hardware queue** — the same tag value is live simultaneously on every
hctx (total in-flight = `nr_hw_queues × queue_depth`, as the driver's own comment
notes). Each hctx routes to its own NVMe queue per member
(`io_queues[hctx->queue_num]`), so two concurrent requests on different queues that
draw the same tag (e.g. both `tag = 2`) both used `rc_volume_prp_va[member][2]` — the
**same host buffer** — and both programmed `prp2 = rc_volume_prp_pa[member][2]` into
their NVMe commands.

They race: request A fills the PRP list, request B overwrites it, and both controllers
DMA from it. The loser's controller walks the *other* request's PRP entries:

- If a stomped entry points at a valid-but-wrong mapped page → the DMA silently
  succeeds to the wrong buffer → **undetected data corruption**.
- If it points at a stale/unmapped IOVA → the IOMMU faults it → the command aborts →
  `EIO`.

The AMD-Vi page fault in the log is the *detectable* case. The silent-corruption case
is the dangerous one — on the **root** filesystem.

Only transfers **> 2 pages** are affected: ≤ 2-page transfers use PRP1/PRP2 directly
and never touch the shared list. And it only manifests under **multi-queue
concurrency**, which is exactly why synchronous `dd` passes and deep-queue fio
O_DIRECT fails.

### Kernel log (the symptom)

```
rcbottom 0000:82:00.0: AMD-Vi: Event logged [IO_PAGE_FAULT domain=0x0013 address=0xf01f1000 flags=0x0050]
rcbottom 0000:82:00.0: AMD-Vi: Event logged [IO_PAGE_FAULT domain=0x0013 address=0xf01f4000 flags=0x0050]
...
rcbottom 0000:82:00.0: AMD-Vi: Event logged [IO_PAGE_FAULT domain=0x0013 address=0xf01f5e00 flags=0x0050]
rcraid: rc_nvme_io_queue_irq: 0000:82:00.0 qid=3 CID=2 op=1 pos=5527728640 len=512 failed SC/SCT=0x0370
I/O error, dev rcraid0, sector 5527728640 op 0x1:(WRITE) flags 0xc800 phys_seg 64 prio class 2
```

The mix of sequential page addresses plus one unaligned address (`0xf01f5e00`) is a
**half-overwritten PRP list** — the fingerprint of one command reading a PRP buffer
another command was concurrently rewriting. `CID=2` on `qid=3` collided with a
`tag = 2` request on another hctx that shared `prp_va[member][2]`.

## Why it only happens under concurrent benchmark load (and not `dd`)

| Test | Command | Concurrency | Result |
|------|---------|-------------|--------|
| Buffered write | `dd … conv=fsync` | QD1 (synchronous) | **OK** |
| O_DIRECT write | `dd … oflag=direct` | QD1 (synchronous) | **OK** — *uses the PRP list, but never two in flight* |
| fio O_DIRECT (KDiskMark default) | deep queue / multiple jobs | many-in-flight across hctx | **FAILED** (`EIO`) |

`dd` issues one I/O at a time, so no two commands are ever in flight to collide on a
shared buffer — regardless of segment count. fio drives a deep queue spread across
multiple hardware queues, so same-tag requests on different hctx collide.

## The fix

Give the pool the missing hardware-queue dimension — `[hctx][member][tag]` — and thread
`hctx_idx` (`hctx->queue_num`) through `rc_volume_build_prp`, `rc_volume_build_io_sqe`,
and `rc_volume_build_discard_sqe`, plus the alloc/free loops. `nr_hw_queues` is computed
before the pool is allocated (and clamped to `RC_VOLUME_MAX_HCTX`) so only the live
`nr_hw × member_count × queue_depth` buffers are allocated (~8 MB, up from ~2 MB).

See `rc_nvme.c` (branch `fix/prp-pool-per-hctx`).

## Environment

- Kernel: `6.17.0-35-generic`
- `rcraid` version: `9.3.2.00255` ("AMD RAID Controller for Linux")
- IOMMU: AMD-Vi, **translated** mode. (The IOMMU is not the bug — it is what *caught*
  the corruption. With `iommu=pt` the faulting DMA would land silently on the wrong
  page instead of being blocked, hiding the corruption rather than fixing it.)
- Array: `rcraid0` (3.6 TB), members = PCI `0000:81:00.0` and `0000:82:00.0`
  (`1022:b000`, driver `rcbottom`). Root fs `rcraid0p2` (ext4).

## Not a KDiskMark bug

KDiskMark/fio correctly reported the `EIO` the driver returned.
