# TRX50 RAID Miniport Notes (Ghidra Recon)

These notes capture what we have learned from the Windows TRX50 StorPort
driver (`rcraid.sys` 9.3.2-00255) so we can implement the same behaviour in
the Linux driver clean‑room style.

## Init Flow Overview

1. **Driver entry** – `FUN_1405da2e6`
   * Calls the vendor helper at `RAX+0x3A0`; on failure it logs and then calls
     `StorPortInitialize`.
2. **Miniport setup** – `FUN_1405da008`
   * Builds an `HW_INITIALIZATION_DATA` structure.
   * Installs the callback table:
     | Callback                              | Function               | Notes                                      |
     |--------------------------------------|------------------------|--------------------------------------------|
     | (likely `HwFindAdapter`)             | `FUN_140008638`        | “front door” routine – handles PCI ID/WMI. |
     | (likely `HwInitialize`)              | `FUN_140008f34`        | Performs BAR discovery and queue bring-up. |
     | Adapter teardown helpers             | `FUN_1400091a4`, `FUN_140009210`, `FUN_14000924c`, etc. |
   * Registers WMI / crash‑dump helpers (`FUN_1405d9008`, `FUN_1405d9080`,
     `FUN_1405d9110`).
   * Feature bitmap at `DAT_1400146B0` is initialised here: `FUN_1405da008` reads a vendor bitmask, ORs it into the global, and later routines gate optional paths (HMB, diagnostics) via `entry+0x2C & DAT_1400146B0`.
   * Calls `FUN_1405d9110`, which uses `MmGetSystemRoutineAddress`/`RtlInitUnicodeString` to populate global service slots: `DAT_140014248` (StorPort service table), `DAT_140014240/250/258/260/268` (ETW + tracing helpers). These pointers back the notification wrappers used across the driver.

## Device Extension Fields

The device extension (`RBX` in most routines) contains a large set of
pre‑allocated offsets. Key ones we observed while the driver maps BARs and
programs doorbells:

| Offset (hex) | Purpose (inferred)                                                                |
|--------------|-----------------------------------------------------------------------------------|
| `0x0010`     | Pointer returned by `MmMapIoSpace` (BAR base).                                    |
| `0x0018`     | Length passed to `MmMapIoSpace`.                                                  |
| `0x00B0`     | Spinlock count used during adapter init.                                          |
| `0x00B5`     | Flags describing the BAR (set according to BAR type).                             |
| `0x16010`    | Pointer passed to miniport service table when programming doorbells.              |
| `0x16020`    | Another doorbell/context pointer (used with service table calls 1…4).             |
| `0x16054`    | Adapter state flag (set/cleared during queue setup).                              |
| `0x16056`    | Unicode-configured nibble (from `FUN_140007d40`, controller variant).               |
| `0x16058`    | Unicode-configured nibble (same as above).                                         |
| `0x1605A`    | Unicode-configured nibble (same as above).                                         |
| `0x1605C`    | Unicode-configured nibble (same as above).                                         |
| `0x16068`    | Queue state indicator (checked before additional BAR processing).                 |
| `0x1606C`    | Queue mode byte selected by `FUN_140007d40` (legacy vs fast path).                |
| `0x1607C`    | Additional state flag enabling optional steps.                                    |
| `0x16100`    | Primary queue dispatcher (rotates through `FUN_140004090` → `FUN_14000FAFC` → `FUN_1400102D8`). |
| `0x16110`    | Callback pointer invoked after spinlock initialisation.                           |
| `0x16120`    | Per-port “disable / quiesce” handler (set by init to routines like `FUN_140003048`). |
| `0x16130`    | Per-port “enable / resume” handler (set by init to routines like `FUN_1400028f8`). |
| `0x16140`    | Status polling helper used by WMI set requests.                                   |
| `0x16148`    | Secondary queue helper (progresses `FUN_140003838` → `FUN_14000D06C` → `FUN_1400102D8`). |
| `0x1C2A0`    | Pointer to an internal descriptor table (`FUN_140008f34` iterates through it).    |
| `0x1C2D8`    | Packed firmware capability word (set by `FUN_140007d40` fallback path).            |

> Callback slots `+0x16108` / `+0x16128` act as a state machine: early init seeds them with wrappers (`FUN_140001438`, `FUN_1400027A8`) and once queues are stable they both point at the thin dispatcher `FUN_1400102D8`/`LAB_14000918C`, which forwards into the steady-state handlers at `+0x16100` / `+0x16148`.

> **Todo:** confirm the purpose of each 0x160xx field by tracing the helper
> routines (`FUN_140008a48`, `FUN_140008bc0`, `FUN_140008d88`,
> `FUN_14000924c`) and rename them accordingly in our adapter struct.

## Callback Behaviour

### `FUN_140008f34` – BAR discovery & mapping

* Retrieves the miniport service table (`*(DAT_140014958 + 0x650)`).
* Iterates adapter resources via service calls @`+0x1C2D0`, storing per‑BAR
  descriptors in local buffers.
* For memory BARs it calls `MmMapIoSpace`, saving the base at `devExt+0x10`
  and the size at `devExt+0x18`. Flag at `devExt+0xB5` tracks the BAR type.
* After mapping, it loops through the BARs with service calls @`0x980`, `0x988`,
  … and either maps additional windows or hands them off to helper routines.
* Invokes `FUN_140008a48` / `FUN_140008bc0` / `FUN_140008d88` /
  `FUN_14000924c` to configure queues, spinlocks, and doorbells.

### `FUN_140008a48` (called from `FUN_140008f34`)

* Obtains a per‑BAR context (offset `0x16288`, `0x1C2E8`).
* Initialises `*(devExt+0x16110)` callback and clears `devExt+0x16054`.
* Calls `FUN_1400093C4` and `FUN_14000273C` to configure queue rings.

### `FUN_140008bc0` / `FUN_140008d88`

* `FUN_140008bc0`:
  * Calls service `+0x650` (alloc adapter extension) to obtain a fresh devExt.
  * Stores config data into a global at `DAT_1402164B0`.
  * Retrieves a port configuration block using service `+0x3F0` (2 times).
  * Zeroes two 0x400-byte scratch buffers (via `FUN_140011400`) then invokes service `+0x3F0` to populate them with wide strings.
  * Service `+0x3F0` returns the descriptor count in `EAX`; the routine subtracts 9 to convert it to the string length passed into `wcsncmp`.
  * Iterates CLSIDs (`"CC_010802"` etc.) to identify controller type, using a `wcsncmp(..., 9)` loop that advances through the table until `EBX == count`.
  * After the loop it replays `wcsncmp` three times against literals (likely `"CC_010802"`, `"CC_010803"`, `"CC_010804"`) and only enters the HMB branch when one is a perfect match.
  * Uses the match result to set `R14B`, then requires both the global feature bitfield `DAT_1400146B0` and the descriptor's local mask at `entry+0x2C` to enable optional paths.
  * Checks `entry+0x29 >= 4` and, when the feature gate passes, takes the worker-name pointer at `entry+0x18` (e.g. `"HMBWORKITEM"`) and calls `FUN_140008368` to queue the Host Memory Buffer work item.
  * `FUN_140008368` is a very thin wrapper around the miniport service pointer stored at `DAT_140014248`, invoked with `DX -> R9W` (controller ID index) and `EDX = 0x2B`. Likely a call into the StorPort notification API (op code 0x2B).
  * `FUN_1400083B9` is the sister wrapper used when only an entry pointer is available: it packages `(entry + 0x20)` and again calls the `DAT_140014248` service thunk.
  * `FUN_14000839C` wraps the more involved WMI registration calls: it zeroes a 0x60-byte work buffer, seeds it with callback pointers (`FUN_140009804`, `FUN_14000a124`, `FUN_140009a14`), and finally invokes the same StorPort service with the buffer at `[RSP+0x20]`. Crash-dump/WMI helpers call this after passing the descriptor gate checks.
  * The global `DAT_140014248` is initialised when `FUN_1405d9110` / `FUN_140010d3c` cache the StorPort service table pointer, so this wrapper is effectively `StorPortNotification(0x2B, ...)`.
  * `DAT_1400140C8` holds the pointer to this descriptor table (at `0x1400140B0`). The table begins with a qword count (`0x28`, i.e. 40 entries), followed immediately by 0x30-byte records. Fields we have identified so far:
    | Offset (hex) | Meaning (inferred)                                             |
    |--------------|----------------------------------------------------------------|
    | `0x00`       | Wide-string CLSID such as `"CC_010802"` (9 chars + NULL).      |
    | `0x18`       | Qword pointer to the worker name literal (`"HMBWORKITEM"`).    |
    | `0x20`       | WMI dispatch helper pointer (points at `FUN_14000e784`).       |
    | `0x29`       | Revision/feature level (must be ≥4 before HMB work queues).    |
    | `0x2C`       | Bitmask checked against `R14B` / `DAT_1400146B0` for gating.   |
  * WMI helper `FUN_14000a124` (registered via `FUN_14000a430`) iterates the same descriptor table when binding GUID handlers, so the `+0x20` dispatch pointer is reused across init and runtime WMI paths.
  * WMI setup helper `FUN_14000e68c` stores that qword count into an `HW_WMI_REGISTRATION_INFO` structure before calling service `+0xBD8`, so the StorPort WMI registration layer is aware of the number of controller descriptors we support.
  * Crash/WMI service `FUN_1405d9448` also walks the table: after the work-item call it checks `entry+0x2C` bit 3 (`0x8`) and requires `entry+0x29 >= 2` before dispatching to `FUN_14000839C`. This confirms those offsets are feature-revision gating for runtime diagnostics.

### `FUN_14000681f` – Adapter object & WMI registration helper

* Allocates/zeroes several scratch buffers, then prepares two constant unicode strings (`0x620060`, `0x00140000`) before calling service `+0x650` (StorPortAllocatePool?).
* Invokes `FUN_14000a3d0` (same handshake used later in init) and services `+0x690`, `+0x210`, `+0x6A0…+0x6B8` to configure device identification strings that feed `RtlIntegerToUnicodeString`.
* Stores the resulting handles into `devExt+0x16020` and updates globals `DAT_140014288`/`DAT_140014290`, suggesting this is the first time the global work-item list is primed. Clears `devExt+0x16050`.
* Calls service `+0x298` to create an object, sets up a 0x50-byte request block with `entry_count = devExt+0x16020` and registers again via `service(+0x2A0)`.
* Invokes `FUN_14000a430` (WMI GUID binder), then issues additional service calls (`+0x428`, `+0xE8`, `+0x188` x3) to finish the registration path.
* Net result: populates the devExt fields (`+0x16020`, `+0x16050`) that the later queue helpers rely on, and seeds StorPort with the WMI dispatch tables.

### `FUN_140009804` / `FUN_14000981e` – WMI request dispatcher

* Builds a stack frame of WMI buffers (`local_80`, `local_88`, etc.) and calls service `+0x850`; the status check indicates only a subset of GUIDs are handled here.
* On success it walks a familiar sequence: service `+0x4E8`, `+0x650`, fetches `devExt->context` at `+0xA8`, and notifies `service(+0x150)` with that pointer.
* Prepares several 0x38-byte descriptors, then calls services `+0x7B8`, `+0x858`, and `+0x860` to push the data back through StorPort (likely `HwWmiQueryDataBlock` style responses).
* Populates a pair of local arrays (`local_90`/`local_b0`) with lengths `0x3`, preserves the caller-supplied buffer pointer, and finally calls service `+0x5D0` followed by `+0x838` to complete the transaction.
* The prologue (`FUN_140009804`) just sets up the SEH frame and shared locals before flowing directly into `FUN_14000981e`, so the two addresses belong to the same handler.
* This path explains why the descriptor table stores per-controller WMI dispatch pointers: the handler pivots through them to answer runtime WMI queries.
* The entry path is provided by `FUN_1400097CC`, a no-op stub that just packages its `(RCX, RDX)` arguments onto the stack so the shared WMI boilerplate can call it via function pointers.

### `FUN_1400093f0` – WMI “set/control” dispatcher

* Sets the global latch at `DAT_140014731` and branches on the WMI method ID (`ECX` ranges 0x65–0x6B).
* For codes 0x65–0x68 it iterates every registered controller (stride `0x1C878` from `RDI`) and invokes per-port callbacks stored in the adapter block:
  * `+0x16120` / `+0x16128` / `+0x16130` handle reset/disable paths.
  * `+0x16110` is called after setting `devExt+0x16054 = 0`, clearing the “active” flag and running a per-port fence.
  * `+0x16100` is the final notifier once the head/tail pointers (`[RDX+0x38]`) convert back into a controller slot.
* Codes ≥0x6A take additional paths: they may grab queued spin locks via `KeAcquireInStackQueuedSpinLock`, invoke `+0x16140`, or toggle the worker list via `+0x16108`.
* These callbacks line up with the `devExt` table documented above: `+0x16120` is the per-port disable/quiesce hook, `+0x16130` is the resume/enqueue hook, and `+0x16140` polls state before optionally setting the “needs attention” flag.
* Whichever branch runs, the routine walks `DAT_1402164b0` entries and always returns through `LAB_140009792`, so it acts as the central “set/control” handler paired with the query dispatcher above.

### `FUN_1400028f8` family – queue enable/disable helpers

* `FUN_140007d40` parses the firmware ASCII/Unicode capability blob, stores four 16-bit values at `devExt+0x16056/58/5A/5C`, sets the queue variant flags (`devExt+0x16068`, `+0x1606C`), and assigns the callback table at `+0x16100`…`+0x16168` based on the detected controller type (doorbells vs legacy path). If the vendor strings match ("NVME", etc.) it selects the full fast-path callbacks; otherwise it points everything at the safe dispatcher `FUN_1400102D8`.
* `FUN_1400028f8` (assigned to `devExt+0x16130`) orchestrates the full “resume” path: it calls `FUN_1400014DC` / `FUN_1400016B0` to allocate SRB contexts, primes doorbell queues via `FUN_140001868`, and reinitialises ring buffers (`FUN_14000273C`, `FUN_1400027DC`).
* Midway it invokes `FUN_140002EF0` to rebuild completion structures, `FUN_140004C10` to push controller settings, and `FUN_14000B500` to post final notifications. This matches the behaviour we see when WMI set requests call back through `+0x16130`.
* During early init the driver temporarily points `+0x16130` at `FUN_14000e494`, a stub that simply stores its `RCX` argument; once the full bring-up completes it swaps in `FUN_1400028f8`.
* The matching disable path `FUN_140003048` (stored at `devExt+0x16120`) reuses many of the same helpers but walks the queue list in reverse, quiescing outstanding requests before WMI can change controller state.
* `FUN_140001438` / `FUN_14000C0BC` / `FUN_1400102D8` form the evolution of the `+0x16108` callback: each is a thin trampoline that just saves its arguments (no hardware work) before handing off to the shared dispatcher once `FUN_1400102D8` is in place.
* `FUN_1400027A8` hands off to `LAB_14000918C` for the `+0x16128` slot; both are tiny wrappers that stash the incoming byte/ptr before jumping into the steady-state handler, suggesting this slot toggles a mode flag rather than touching hardware directly.
* `FUN_140001ED8` is the legacy pathway invoked when `DAT_1400146B0` bit0 is set; it funnels straight into `FUN_14000924C`, effectively reusing the modern queue activation code for older firmware.
* Smaller helpers (`FUN_1400014DC`, `FUN_1400016B0`, `FUN_140001868`, etc.) are thin wrappers that zero local state, run shared allocation routines, or iterate controller arrays—their presence here explains why the WMI dispatcher needs a broad set of callback slots per port.
* `FUN_140003048` calls into `FUN_140001318`/`FUN_14000330C` to snapshot queue pointers, and `FUN_14000403C` to flush outstanding SRBs; when it reaches the tail it invokes `FUN_14000330C` again to hand off to firmware before returning.
* Queue mode values: `FUN_140007d40` writes `devExt+0x16068`=`0x63` plus increments when the firmware strings match NVMe/other GUIDs; bit0 of `devExt+0x1606C` selects whether to install the fast-path or safe dispatcher.
* The steady-state dispatcher (`FUN_1400102D8`) ends up installed both at `+0x16100` and `+0x16148` when the firmware falls back to the safe path (`FUN_140007d40` sets this when no string match occurs), so once init finishes the `+0x16108` / `+0x16128` trampolines simply marshal arguments into shared doorbell routines.
* `FUN_14000924C` (queue activation) relies on `KeStallExecutionProcessor` between doorbell writes, so firmware expects short busy-waits during bring-up.

### `FUN_14000a430` – WMI GUID binder

* Clears a 0x60-byte registration block via `FUN_140011400`, then fills three callback slots with `FUN_140009804` (query), `FUN_14000a124` (enumerator), and `FUN_140009a14` (set/control).
* Uses `PTR_LOOP_1400140a0` to step through each descriptor entry (`+0x30` stride). For entries whose feature bits (`+0x2C & 0x8`) and revision (`+0x29 >= 2`) allow it, pulls the HMB worker string at `+0x18` and re-invokes `FUN_14000839C` to publish the callbacks through `StorPortNotification(0x2B, …)`.
* When the initial `StorPort` registration succeeds, it re-runs the sequence with `local_64 = 3`, indicating a second registration stage (probably “set” vs “query” GUID binding).
* This binder is called both during adapter bring-up (`FUN_14000681f`) and when the miniport’s WMI helper (`FUN_14000a124`) refreshes the GUID list; the descriptor table therefore centralises every GUID/handler pair our Linux port will have to mirror.

### `FUN_14000a124` – Descriptor enumerator for WMI

* Walks `PTR_LOOP_1400140a0` using the descriptor count at `DAT_1400140B0`, copying each entry into caller-provided buffers and handing control to `FUN_14000e784` via the pointer at `entry+0x20`.
* For entries where the feature mask (`+0x2C & 0x8`) and revision (`+0x29 >= 2`) pass, it chains into `FUN_14000839C`, giving the GUID binder another chance to register HMB-related handlers.
* Returns the number of descriptors processed, so the caller can size the GUID arrays and work-item queues correctly.
  * Installs several miniport callbacks:
    - `local_8C0` → `FUN_140008f34` (subcreate)
    - `local_8B8` → `FUN_14000911C` (unmap BAR)
    - `local_8E0` → `FUN_140008a48` (queue init)
    - `local_8D0` → `FUN_140008BC0` (this function)
    - `local_8D8` → `FUN_140008B44` etc.
  * Calls service `+0x1B8` (StorPortGetDeviceBase?) and `+0x400` (PostDeviceInfo).
  * Calls `FUN_140005FF4` to build SRB list, `FUN_140007BA0` to parse devices,
    and `FUN_14000A3D0` to do final handshake.
  * On success writes to `devExt+0x8`, `+0x18` etc., maps BAR memory via `MmMapIoSpace`.
  * Ties into service `+0x980`/`+0x988` for final doorbell enables.
* `FUN_140008d88` (not yet fully documented) handles adapter object creation and service registration—track in future passes.

### `FUN_14000924c`

* Marks the adapter active (`devExt+0x16054 = 1`, `devExt+0x1607C` flags).
* Issues four doorbell writes by calling `service(+0x188)` with indices 1…4.
* If `devExt+0x16068 == 1`, also calls `FUN_140001ED8` (legacy path).
* `FUN_14000924c` sequence (queue activation):
  * Begins by calling service slot `+0x650` with `(StorPortRegister?)` then sets `devExt+0x16054 = 1`. Immediately loads `ECX=0x1388` (decimal 5000) and calls `KeStallExecutionProcessor`, so the legacy path is separated by a 5 µs stall.
  * If `devExt+0x16068 == 1` it calls `FUN_140001ED8` and then enforces a second stall with `ECX=0x61A8` (decimal 25000).
  * If `devExt+0x1607C` was zero it calls `FUN_14000A564` (WMI/descriptor binder) before marking `devExt+0x1C2DC = 1`.
  * Rings the firmware doorbells by loading `devExt+0x16020` into `RDX` four consecutive times and invoking service `+0x188` (indices 1..4 in the Windows driver). Each call is separated only by service latency; no explicit delay between them in this block.
  * Returns immediately afterward.

## Remaining Unknowns

* Exact mapping between each service table call (`+0x650`, `+0x1B8`, `+0x1F8`,
  `+0x188`, `+0x9D8`, etc.) and the hardware register/operation. Need to
  translate to Linux equivalents (StorPortGetDeviceBase, BuildIoCaps, etc.).
* Bit semantics of the global mask at `DAT_1400146B0` and how it interacts with
  the per-descriptor flag fields (`offsets 0x29/0x2C`) that gate the HMB path.
* Which callback corresponds to `HwResetBus`, `HwAdapterControl`, etc.
* Interrupt handling path and completion processing (not yet analysed).

## Porting Plan

1. **Mirror the BAR mapping** in `rc_bottom.c`:
   * Walk PCI BARs, iomap, and populate our adapter struct using the offsets
     above.
   * Release the mappings in `rc_bottom_cleanup`.
2. **Translate queue setup** by re‑implementing the logic from
   `FUN_140008a48`, `FUN_140008bc0`, `FUN_140008d88`, and `FUN_14000924c`.
3. **Hook blk‑mq** to the real queues so request submission uses the TRX50
   doorbells instead of the in-memory stub.
4. **Document/rename remaining offsets** as we decode them (e.g. interrupt
   processing, SMART/WMI functions).

With the BAR layout documented we can now replace the stub code in the Linux
driver and start issuing real hardware commands.

## Callback Table Seeds (FUN_1400079a4)

* `rcbottom` seeds the miniport callback table by storing function pointers into the device-extension block around offset `+0x8C0`.
* `FUN_1400079a4` writes the pointer for `FUN_140008f34` (BAR bring-up) into `devExt+0x8C0`, alongside neighboring slots that hold teardown and WMI handlers.
* The Windows driver populates this table before any BAR discovery; Linux port must mirror the same struct layout so the dispatcher trampolines (`FUN_140001438`, `FUN_1400027A8`, etc.) can be swapped in later phases.

### `FUN_1400079a4` – Front-door registration loop

* Entry: `(RCX=devExt, RDX=descriptorPtr, R8=serviceCtx)`; pulls the port count from `*(descriptor+6)` and defaults to 1 when zero.
* For each port:
  * Zeroes a 0x68-byte stack block via `FUN_140011400` and seeds callback slots: `local_e0=FUN_14000751c`, `local_d0=FUN_1400074d4`, `local_c8=FUN_1400073e0`, `local_d8=FUN_140007428` (unless `DAT_140014280!=0`, in which case the pointer is nulled and `devExt+0x160fc` bit0 is OR’d in).
  * Builds the registration struct at `devExt + (devExt+0xB0 + 6) * 8` and caches the descriptor pointer (`local_b8=param_2`) plus third argument (`local_b0=param_3`).
  * Calls StorPort service `+0x468` with `(serviceCtx, devExt->context@+0x20, local_f8, 0)`; on success increments `devExt+0xB0` and stores a secondary 0x40-byte block through service `+0x4A8` using the previous entry from `devExt+0x30[...]`.
  * Updates `devExt+0xB4` based on the returned status byte (`local_58`).
* Loop decrements `R14D` until every port record is registered, then returns.
* Takeaway: Linux adapter struct must expose the fields at `+0x20`, `+0x30`, `+0xB0`, `+0xB4`, `+0x160fc`. We also need equivalents for the Windows callback trio (`FUN_14000751c` etc.) when translating these registrations to blk-mq or worker initialization.

## BAR Discovery (FUN_140008f34 – updates)

* Entry arguments: `RCX=devExt`, `RDX=serviceTable`, `R8=HAL context`.
* First steps cache `R8` into `[RSP+0x18]` and call the StorPort service table to enumerate adapter resources.
* `MmMapIoSpace` is invoked after locating the memory BAR entry; result stored at `devExt+0x10` (base) with length in `devExt+0x18`. Flag byte at `devExt+0xB5` tracks BAR type.
* After mapping, the routine fans out into:
  * `FUN_140008a48` – queue spinlock bootstrap (writes callbacks at `+0x16110` and clears `+0x16054`).
  * `FUN_140008bc0` – descriptor/WMI table registration, gated by feature bits.
  * `FUN_140008d88` – adapter object creation and StorPort registrations.
  * `FUN_14000924c` – doorbell activation once queues are ready.
* Each StorPort service slot (`+0x1B8`, `+0x3F0`, `+0x980`, etc.) must be mapped to Linux equivalents (pci_resource, dma_alloc, etc.).

### `FUN_140008d88` – Adapter object creation

* Issues StorPort service `+0x650` to retrieve the adapter context, then calls the function pointer at `devExt+0x16158`. This looks like the Windows miniport wiring the Adapter Control or reset callback into the device extension.
* Clears `devExt+0x1C2DD` on success. Linux equivalent should register whatever control path maps to the callback we eventually decode at `+0x16158`.

### `FUN_140008dd8` – No-op stub

* Simply zeroes `EAX` and returns after writing the arguments into stack locals. Acts as a placeholder callback in the Windows driver; we can leave the matching slot empty until a feature gate requires it.

### `FUN_140008dec` – Worker pool sizing & queue enumeration

* Queries `KeQueryMaximumProcessors()` then invokes service `+0x650` to fetch adapter state. Determines the maximum number of worker threads (`EBX`) based on CPU count, `devExt+0x16068`, and bounds of 8/16 controllers.
* Calls service `+0x910` to allocate a handle array, then runs nested loops:
  * Outer loop: index into `+0x918` (per-port context list) and service `+0x950` to fetch a descriptor; stores pointer in `R12`.
  * Inner loop: iterates service `+0x958` (per-entry accessor) and checks a status byte at offset `+0x1`. When the byte is `0x02` it tracks completions, and once the threshold is exceeded, calls service `+0x960` to purge/reset the entry.
* Loop exit increments counters and repeats until all handles are processed. These services likely correspond to Windows worker queues; when porting, we’ll need Linux workqueues/kthreads sized by CPU count and capped like the Windows logic.

## Queue Activation Path (FUN_140001ed8 / FUN_14000924c)

* Legacy helper `FUN_140001ed8` installs `FUN_14000924c` into the `devExt+0x16100` queue dispatcher when `DAT_1400146B0` bit0 is set (legacy compatibility mode).
* `FUN_14000924c` marks the adapter active (`devExt+0x16054 = 1`, sets `devExt+0x1607C` flags) and writes doorbells via StorPort service `+0x188`, calling it 4× with indices 1..4.
* Between each doorbell write the driver calls `KeStallExecutionProcessor`; we need the literal stall values from the disassembly to replicate timing on Linux (likely small µs delays).
* When `devExt+0x16068 == 1` the routine invokes `FUN_140001ED8` (legacy queue bring-up) before proceeding.
* `FUN_140001ed8` walks 0x30 controller slots, stepping `devExt` substructures in 0x728-byte strides. For each slot with a non-null context at `slot+0x6e0`, it:
  * Optionally calls the StorPort thunk at service offset `+0x9e0` when the global latch `DAT_140014731` is clear (looks like an "acquire lock" wrapper).
  * Clears the per-port queue tables stored at `[slot + index*8 + 0x8]` and `[slot + index*8 + 0x108]` after calling `FUN_140001008` to flush outstanding work and release any heap (`[RAX+0x838]`).
  * Uses `FUN_1400097ac` between entries, then zeroes slot-level pointers at `+0x2A4`/`+0x2B0` before releasing the lock via service `+0x9E8`.
  * The outer loop index lives in `EDI`; the inner loop walks the port count stored at `[RSI + 0x158fd]`, so expose both fields when we model the Linux adapter.
* These loops confirm the Windows driver expects up to 0x30 (48) controller records and that each record owns an array of 0x18 queue pointers. The Linux port needs matching allocation/layout to reuse the later dispatcher trampolines.

### Descriptor accessor @`devExt+0x1C2D0`

* Installed during `FUN_140007d40` when the driver parses the PCI hardware ID strings. The same routine also seeds the callback arrays at `+0x16100`…`+0x16168` and caches vendor-specific fields at `+0x16056/58/5A/5C`.
* Uses StorPort service `+0x418` plus the descriptor blob at `DAT_140012258` to expose a generic accessor `(table, flags, outBuf, index, width)`. The blob content we captured from Ghidra (little-endian qwords) is:

  ```0000:0058:DAT_140012258
  4b0a88f82eb3deb1
  11d06f25496b8280
  2f09e22b0008afbe
  0000000000000000
  0000000500000005
  0000000500000005
  0000000000000080
  481f895fdcaf9c10
  3356f5d2ced492a4
  40880b3a585d326b
  be58690fb0883989
  481f895fdcaf9c10
  3356f5d2ced492a4
  444b2b37d52ce820
  7bd091e5920033a6
  48e8f89b2b9443ac
  5afd6ec9b62c92b2
  0000000000000000
  0000000000000000
  36a1b77f40298335
  b8afc6fc52255940
  0000000000000080
  32a407e392d8e5b7
  b62a445f63539d2f
  11d0ba97b091a08a
  2ab3b700aa0014bd
  0000000000000080
  524f5744444f505a
  0000004d4554494b
  4f57594649544f4e
  ```

  * Entry 0 (bytes 0x00–0x2F): header words, likely counts/strides for the accessor. The sequence `0x00000005` repeated four times looks like an opcode table; `0x80` may be the record stride (128 bytes) used later.
  * Entries 1–4 (0x30-byte chunks) each start with a GUID-like ASCII block ending in `"ZPODDWORKITEM"` / `"NOTIFYWO"`. These correspond to work-item descriptors the driver registers via service `+0x418` and the accessor at `devExt+0x1C2D0`.
* Opcode behaviour confirmed from call sites:
  * `0x11`: fetch 16-bit register descriptor; accessor is re-invoked at `header+2` to read the associated data block into `local_30`.
  * `0x05`: mirror of `<0x11>`, stores into `local_2C`.
  * `0x10`: multi-controller path to pull queue depth nibble/slot count (written into `devExt+0x1C7A8/AC`).
* `FUN_140007d40` uses offsets `sVar5`/`sVar3` with this accessor to populate `devExt+0x1C2D8`—the packed firmware capability word consumed by `FUN_140008f34` and queue setup.
* Call pattern from `FUN_14000807x`/`1400081Fx`/`140008F9x` confirms the Windows x64 argument order: `RCX = *(devExt+0x1C2A0)` (table handle), `RDX = flags` (zero in observed calls), `R8 = &out`, `R9D = index/opcode selector`, with the 5th argument placed on the stack via `local_108`/`local_48`. The accessor writes 16-bit or byte results into the caller-provided buffers (`local_e4`, `local_e8`) that are then decoded (e.g., low byte compared against `0x10`, high byte used as next cursor).
* Linux port needs a matching helper that can return vendor capability words, queue depth/width pairs, and feature bits so higher-level code can mirror the Windows behaviour.

> **Need from Ghidra:** In the Listing view, search for `mov [rbx+0x1c2d0],` to locate the store that seeds this function pointer, open that callee, and dump its disassembly. That routine (the “descriptor accessor”) walks the `DAT_140012258` blob and implements opcodes `0x05`, `0x10`, `0x11`—we still need its exact control flow to finish decoding the descriptor format.