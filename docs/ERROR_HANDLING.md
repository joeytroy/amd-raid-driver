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

## Limits (out of scope for this pass)

These are the next problems on the list, in roughly the order they
should be tackled:

- **Controller reset path.**  Disable CC.EN, wait CSTS.RDY=0, re-init
  admin queue, re-create I/O queues, re-arm IRQ, replay or fail
  in-flight.  Lots of race exposure (IOMMU state, blk-mq quiesce, MSI
  re-arm); deserves its own commit.
- **Retry on transient SCs.**  Distinguish DNR=0 (retryable) from DNR=1
  (don't).  Doesn't make sense without reset above — we'd just
  re-dispatch to a controller we already declared dead.
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
- **After**: stuck commands time out at 30 s.  The volume goes offline
  (every subsequent dispatch returns `BLK_STS_IOERR`) and every
  in-flight request fails promptly.  Module reload required to get the
  volume back.

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
