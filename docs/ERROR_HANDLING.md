# Error Handling & Timeouts

How the I/O path detects, contains, and reports failures.  Live in
`rc_nvme.c`; this document explains the why so a reader can navigate the
code without reverse-engineering the design from individual functions.

## Scope

In this pass:

- blk-mq timeout callback that fires at 30 s (the blk-mq default).
- Per-adapter `dead` flag with three ways to set it.
- Best-effort NVMe Abort on stuck commands.
- Tagset-wide drain of in-flight requests when an adapter dies.
- Decoded SC/SCT/DNR/More in failure logs.

**Not in this pass** — see "Limits" at the bottom:

- Controller reset & re-init.
- Retry of transient errors.
- Recovery for the sync init-time helpers (`rc_nvme_admin_cmd`,
  `rc_nvme_io_cmd_sync`).

## Adapter health states

Every adapter is either **alive** or **dead**.  The flag lives in
`adapter->ctx.nvme.dead` (`rc_linux.h:struct rc_nvme_state`).  It starts
zero (kzalloc), transitions to `true` once, and is never cleared.  Read
with `READ_ONCE`, written with `WRITE_ONCE`.

Three transitions to dead:

1. **`rc_nvme_irq` health canary.**  Every ISR entry reads `CSTS` first.
   If `CSTS == 0xffffffff` (device hot-unplugged or PCI vanished) or
   `CSTS.CFS` (controller fatal status) is set, the CQ contents are no
   longer trustworthy.  Flag is set, both wait queues are woken so any
   sync-init helpers return promptly, and the ISR returns without
   touching the CQ.  Hardirq-safe — no sleeping, no work scheduling;
   the next `.timeout` invocation handles cleanup.

2. **`rc_nvme_check_dead` from `.timeout`.**  Process-context probe that
   re-reads CSTS.  Same predicates as the ISR canary, used by
   `rc_volume_timeout` to decide whether a stuck request is just slow or
   actually unreachable.

3. **`rc_volume_timeout` after a "stuck" command.**  Even if CSTS still
   reads sane, an unresponsive command without a reset path means we
   cannot safely recycle its CID (see "CID-recycle hazard" below).  The
   timeout handler flags the involved adapter dead as part of its
   response.

## I/O path participants

Five functions cooperate.  Each has one job:

| Function | Context | Job |
|---|---|---|
| `rc_volume_queue_rq` | submitter | Fast-fail if `rc_volume_any_member_dead()` before touching the SQ.  Saves us from wedging more CIDs against a controller we no longer trust. |
| `rc_nvme_irq` | hardirq | CSTS canary first.  Then walk the CQ and call `blk_mq_complete_request` per CID. |
| `rc_volume_complete` | softirq | Decode `pdu->sc_sct`.  For real NVMe failures, log SCT/SC/DNR/More.  For the `RC_VOLUME_SC_DEAD` sentinel, log "controller dead".  End the request with `BLK_STS_IOERR` (failure) or `BLK_STS_OK` (success, with READ memcpy first). |
| `rc_volume_timeout` | workqueue | The recovery decision tree; see next section. |
| `rc_volume_drain_dead` | workqueue | Walk `blk_mq_tagset_busy_iter`, complete every in-flight request that touched a dead adapter with the sentinel SC. |

## Timeout flow

Triggered when a request's CQE hasn't landed within 30 s.  Returns
`BLK_EH_DONE` in every path — blk-mq will not retry, the driver has
taken responsibility for ending the request.

```
.timeout(req)
 │
 ├── blk_mq_request_completed(req)?      ─yes→ BLK_EH_DONE  (ISR raced and won)
 │
 ├── rc_nvme_check_dead(involved members)?
 │   │
 │   yes ────────────────────────────────→ drain_dead()
 │                                          → end_request(IOERR)
 │                                          → BLK_EH_DONE
 │   no
 │   │
 ├── issue rc_nvme_abort() to involved members  (best effort)
 │
 ├── blk_mq_request_completed(req)?      ─yes→ BLK_EH_DONE  (abort completed it)
 │
 └── WRITE_ONCE(involved.dead = true)
     → drain_dead()                            (drain takes this req with it,
        → end_request(TIMEOUT)                  unless drain already did)
     → BLK_EH_DONE
```

"Involved members" depends on op:
- `REQ_OP_READ` / `REQ_OP_WRITE` / `REQ_OP_DISCARD` — just `pdu->member_idx`
  (stripe-mapped at submit).
- `REQ_OP_FLUSH` — all members (FLUSH is fan-out, can't complete unless
  every member ACKs).

The `blk_mq_request_completed` re-check after Abort is important: the
Abort admin command may have caused the controller to post the original
CQE (with SC = "Aborted by Request").  If so the ISR ran during the
abort and we shouldn't end-request the same tag twice.  blk-mq's
state machine would catch the double-end, but skipping it cleanly is
cheaper than relying on that.

## CID-recycle hazard

This is the core reason `.timeout` is so aggressive about flagging
adapters dead.

When `blk_mq_end_request(req, BLK_STS_TIMEOUT)` returns, blk-mq frees
the request and the tag (the integer that became `CID` in the SQE) is
eligible for reuse on the next dispatch.  If the controller eventually
posts a CQE for that CID — late, after a hang, but real — the ISR's
`blk_mq_tag_to_rq(tags, cid)` will look up whatever request currently
holds the tag, which may be a completely unrelated I/O.

For a READ the ISR memcpys the per-tag DMA buffer (containing data the
controller wrote for the original LBA) into the new request's bvecs.
**Silent data corruption.**  For a WRITE the request just completes
with a stale status, which is "merely" a wrong success indication.
Either way the only safe option without a reset path is to stop
dispatching to that controller and drain anyone already in flight.

A real controller-reset implementation can recover the CID space
(disable + re-enable the controller cleanly, the CID counters reset)
and let the volume keep serving.  Until that exists, `dead` means
permanently dead, until module reload.

## Drain protocol

`rc_volume_drain_dead` is the cleanup pass.  Called from `.timeout` and
nowhere else (the ISR is hardirq-context and can't run it).

1. Build `dead_mask` — one bit per member, set if `nvme.dead`.
2. If mask is zero, no-op.
3. `blk_mq_tagset_busy_iter` calls `rc_volume_drain_iter` for every
   in-flight request.
4. For each request, `drain_iter` decides "would this request need a
   dead member to complete?":
   - FLUSH: any dead member ⇒ yes (flush needs all).
   - R/W/DISCARD: `member_idx ∈ dead_mask` ⇒ yes.
5. If yes and not already completed, write `pdu->sc_sct = RC_VOLUME_SC_DEAD`
   and call `blk_mq_complete_request`.

`blk_mq_complete_request` is atomic against the ISR's competing call —
only one will win the request's state-machine transition.  So a CQE
arriving naturally while we're iterating completes safely; our drain
becomes a no-op for that one request and continues for others.

## Sentinel SC

`RC_VOLUME_SC_DEAD` (`0x7fff`) is the value stored in `pdu->sc_sct` by
drain to distinguish "we killed this request because the controller is
dead" from "the controller returned an NVMe error".

Layout of `pdu->sc_sct` (CQE.status >> 1):

```
bit 14: DNR (Do Not Retry)
bit 13: M   (More — extended info via Get Log)
bits 10:8 : SCT (Status Code Type, 3 bits)
bits  7:0 : SC  (Status Code, 8 bits)
```

`0x7fff` sets every bit: DNR=1, More=1, SCT=7 (vendor), SC=0xff
(vendor).  A spec-compliant controller will never post this combination
for a real command, so the value is reliably distinct.

`rc_volume_complete` checks for the sentinel first and prints
`"controller dead"` instead of decoding it as NVMe status.

## admin_mutex

`rc_nvme_admin_cmd` is now serialized by a per-adapter
`nvme.admin_mutex`.  Held across SQE write, doorbell ring, CQE wait,
and CQ-head advance.

Before this change the admin queue was only used at module init
(single-threaded by definition).  Now `.timeout` can issue an Abort
from a workqueue while teardown or another timeout is touching the same
admin queue.  Mutex makes that race safe.

CID is still hardcoded to `0` — only one admin command is in flight at
a time, guaranteed by the mutex.

## Controller reset (manual)

`rc_nvme_reset_controller(adapter)` is the recovery path out of the
`dead` state.  Triggered by the operator writing `1` to the per-adapter
sysfs attribute:

```
echo 1 | sudo tee /sys/bus/pci/devices/0000:81:00.0/rcraid/reset
```

The path returns 0 on success or `-errno` on failure (`-EIO` if the
controller refused to come back ready, `-ENOTSUPP` for non-NVMe-mode
adapters, `-EINVAL` for any input other than `1` or `1\n`).

Sequence under `admin_mutex`:

```
1. WRITE_ONCE(dead = true)       (idempotent — usually already true)
2. blk_mq_quiesce_queue          no new .queue_rq runs
3. rc_volume_drain_dead          fail in-flight; their CIDs are about
                                  to be wiped by CC.EN=0
4. disable_irq                   wait for in-flight ISR to finish so
                                  it doesn't race the register writes
5. INTMS = ~0, CC.EN = 0, wait CSTS.RDY = 0
6. memset SQ/CQ buffers; reset tail/head/phase
7. Reprogram AQA/ASQ/ACQ, CC.EN = 1, wait CSTS.RDY = 1
8. INTMC = ~0, enable_irq        admin commands need ISR wakes
9. Create I/O CQ + Create I/O SQ on the existing DMA buffers
10. WRITE_ONCE(dead = false)
11. blk_mq_unquiesce_queue       new I/O resumes
```

Any failure between steps 5 and 9 returns `-EIO` with `dead` still set;
the adapter stays offline and module reload becomes the only recovery.

Things the reset deliberately does NOT do:

- **Re-issue Identify Controller / Namespace.**  CAP/MDTS/namespace
  values shouldn't change across a reset on the same silicon.  If they
  do (genuine hot-swap, different firmware loaded out-of-band), that's
  a different recovery problem and Identify-divergence detection
  belongs there.
- **Re-allocate DMA buffers.**  Admin SQ/CQ, I/O SQ/CQ, and the
  per-tag `rc_volume_dma_va[][]` pool all persist.  The DMA handles
  are still valid across CC.EN=0; the controller just forgot about
  them and needs them re-bound via Create I/O CQ/SQ.
- **Touch the gendisk or tagset.**  Only controller-side state moves.
  blk-mq tags get re-dispatched to new requests post-reset and reuse
  the same per-tag buffers.

The split between locked-inner and public-wrapper admin-command paths
(`__rc_nvme_admin_cmd_locked` vs `rc_nvme_admin_cmd`) exists so reset
can hold `admin_mutex` across steps 5-9 without deadlocking when it
issues the Create I/O CQ/SQ admin commands in step 9.

## Automatic reset on timeout

`.timeout` now schedules `rc_nvme_reset_controller` automatically for
each adapter it flags dead.  The mechanism is one `work_struct` per
adapter (`nvme.auto_reset_work`) plus a `nvme.auto_reset_disabled`
latch.

Flow:

```
.timeout marks adapter dead, drains in-flight
   │
   └── rc_volume_schedule_auto_reset_for_req(pdu)
        │
        └── for each involved member, if dead && !auto_reset_disabled:
             schedule_work(&nvme->auto_reset_work)
             │
             └── rc_nvme_auto_reset_fn (workqueue context)
                  │
                  ├── if !dead anymore (e.g. operator beat us with
                  │   sysfs reset), return — nothing to do
                  │
                  └── rc_nvme_reset_controller(adapter)
                       │
                       ├── success: dead = false, auto_reset_disabled = false
                       │   → new I/O resumes automatically
                       │
                       └── failure: auto_reset_disabled = true
                          → controller stays dead, no further auto attempts
                          → operator must run sysfs reset to recover
```

Policy is **one auto-reset attempt per death episode**:

- **Episode**: from the moment `dead` flips true until either reset
  succeeds (clearing both flags) or reset fails (latching
  `auto_reset_disabled`).
- During an episode `schedule_work` is the only entry point and the
  workqueue's own "already queued" check coalesces multiple
  `.timeout` invocations into a single attempt.
- After a **successful** reset both flags are clear, so a fresh death
  later (controller dies again hours from now) gets its own attempt.
- After a **failed** reset `auto_reset_disabled` is latched; further
  `.timeout` invocations drain and end requests but don't re-attempt
  reset.  Operator runs `echo 1 > .../rcraid/reset`; a successful
  manual reset clears the latch, restoring auto-recovery.

This avoids the obvious thrash failure mode (genuinely-fried silicon →
30 s timeout → 50 ms reset → timeout → reset → ...) while still
self-healing the common cases (a single hung command, a transient
fault) without operator intervention.

`cancel_work_sync(&nvme->auto_reset_work)` is the first thing
`rc_nvme_cleanup_controller` does, so an in-flight auto-reset finishes
before module teardown frees admin queues / MMIO.

## Limits (out of scope for this pass)

These are the next problems on the list, in roughly the order they
should be tackled:

- **Retry on transient SCs.**  Distinguish DNR=0 (retryable) from DNR=1
  (don't) and re-dispatch the failed request rather than ending it.
  Lives on top of reset (a recovered controller is the right place to
  retry).
- **Abort path for sync init helpers.**  `rc_nvme_admin_cmd` and
  `rc_nvme_io_cmd_sync` already time out at 2 s and surface `-ETIMEDOUT`
  to their callers (`rc_nvme_init_controller` and friends).  No caller
  currently needs to soldier on past that, so no recovery code exists
  for them.  Add when one does.
- **Per-CPU I/O queues.**  Multiple hctxs means the ISR's tag lookup
  must also know the qid.  Belongs in the multi-queue commit, not here.

## Behaviour change summary for users

The visible difference from the prior behaviour:

- **Before**: a stuck command wedged its tag forever and printed a
  ratelimited warning on every subsequent attempt.  No way to recover
  short of reboot.
- **After**: stuck commands time out at 30 s and trigger an automatic
  controller reset.  The offending request still fails with
  `BLK_STS_TIMEOUT`, but the controller is back online ~50 ms later
  and subsequent I/O succeeds without operator intervention.  If the
  auto-reset itself fails (CSTS.RDY never comes back, Create I/O CQ
  fails, etc.), the controller stays dead and `echo 1 >
  .../rcraid/reset` is the manual escape hatch.

If you see the volume go offline unexpectedly, check `dmesg` for one of:

```
rcraid: rc_nvme_irq: 0000:..:.. controller dead (CSTS=0x........) — failing in-flight I/O
rcraid: rc_volume_timeout: tag=N op=M: member dead — draining in-flight
rcraid: rc_volume_timeout: tag=N op=M: issuing NVMe Abort
```

The first is the ISR catching CSTS.CFS or a hot-unplug.  The second is
`.timeout` discovering a dead controller while investigating a stuck
request.  The third is the "controller still looks alive but this
command is hung" path.
