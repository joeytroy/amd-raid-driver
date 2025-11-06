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
| `0x0670`     | Scratch pointer cleared by ISR teardown (`FUN_14000d29c`).                        |
| `0x0678`     | Count of spinlocks in array at `+0x6c0` (cleared by `FUN_14000d29c`).             |
| `0x0698`     | Memory pool pointer (DMA buffer) freed by `FUN_14000d29c` with tag `0x72635041`.  |
| `0x06B8`     | Primary queue spinlock handle released by `FUN_14000d29c`.                        |
| `0x06C0`     | Array of per-queue spinlock handles freed by `FUN_14000d29c`.                     |
| `0x16010`    | Pointer passed to miniport service table when programming doorbells.              |
| `0x16020`    | **Adapter/controller handle** - Primary adapter context pointer used for doorbell operations, adapter context retrieval, and adapter linkage. Set from `devExt+0x16018` in `FUN_1400067fc` after service `+0x258` call. Used with services `+0x650` (get adapter context), `+0x188` (doorbell writes), `+0x6d8` (cleanup). Also used for adapter traversal in multi-adapter scenarios (`FUN_140006e3c`, `FUN_14000a564`, `FUN_14000a72c`). |
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
| `0x15f80`    | Spinlock handle for work item queue (used by `FUN_14000e960` for deferred processing). |
| `0x15f88`    | Head pointer to work item queue (linked list of deferred SRB commands when queue is full). |
| `0x15f90`    | Tail pointer to work item queue (linked list of deferred SRB commands when queue is full). |
| `0x15948`    | Array of queue handles (indexed by `devExt+0x15968`). |
| `0x15968`    | Queue index selector (used to index into queue handle array at `devExt+0x15948`). |

> Callback slots `+0x16108` / `+0x16128` act as a state machine: early init seeds them with wrappers (`FUN_140001438`, `FUN_1400027A8`) and once queues are stable they both point at the thin dispatcher `FUN_1400102D8`/`LAB_14000918C`, which forwards into the steady-state handlers at `+0x16100` / `+0x16148`.

> **Todo:** confirm the purpose of each 0x160xx field by tracing the helper
> routines (`FUN_140008a48`, `FUN_140008bc0`, `FUN_140008d88`,
> `FUN_14000924c`) and rename them accordingly in our adapter struct.

## StorPort Service Slots (confirmed)

| Offset | Purpose (Windows behaviour) |
|--------|-----------------------------|
| `+0x188` | Rings firmware doorbells (indices 1…4) during queue activation. |
| `+0x1B8` | Consumes the static descriptor block at `0x140012C20` while programming BAR/doorbell templates. |
| `+0x1F8` | `FUN_14000d66c` – allocates a 0x78-byte queue descriptor (`FUN_14000c2fc`) and populates per-queue DMA tables. |
| `+0x338` | Memset helper implemented by `FUN_140011400`. |
| `+0x3F0` | Allocates per-queue DMA/control blocks (`FUN_14000655a`, arguments `(ctx, outPtr, queueIndex, 0x400)`). |
| `+0x418` | **Descriptor accessor initialization**: Initializes descriptor accessor function pointer using static blob `DAT_140012258`. Call: `service(+0x418)(StorPortContext, devExt+0x20, &DAT_140012258, devExt+0x1c298)`. Sets `devExt+0x1c2d0` (accessor function) and `devExt+0x1c2a0` (accessor context). Also used for per-queue DMA/control block allocation in other contexts. |
| `+0x650` | Returns adapter context. Called as `(serviceCtx, devExt, PTR_DAT_140014090)` where `PTR_DAT_140014090` points to `DAT_140014078` (value `0x28` = 40). |
| `+0x680` | Completion pump invoked by the ISR to drain queue handles. |
| `+0x838` | Manages completion handles (allocate/free paths in `FUN_14000c900`). |
| `+0x980` | Returns iteration bound/count during resource enumeration. |
| `+0x988` | Enumerates queue/doorbell descriptors after `+0x980`. |
| `+0x9D8` | `FUN_1400023BB` – configures completion register blocks twice per queue during setup. |
| `+0x1C2D0` | Descriptor accessor used when parsing firmware/static tables. |

## Global Constants and Data Structures

### `PTR_DAT_140014090` / `DAT_140014078`

**Location**: Global pointer in `rcbottom.sys`

**Value**: `DAT_140014078 = 0x28` (40 decimal)

**Usage**: 
* Referenced in 24+ functions across the driver
* Used as the 3rd parameter in service slot `+0x650` calls: `(serviceCtx, devExt, PTR_DAT_140014090)`
* Example from `FUN_140008f34`: `(**(code **)(DAT_140014958 + 0x650))(DAT_140014980, param_1, PTR_DAT_140014090)`

**Possible meanings**:
* Adapter count or maximum adapters (40 = 0x28)
* Queue configuration constant
* Port count or configuration parameter
* Size of some internal structure

**Note**: This constant is passed to the adapter context retrieval function, suggesting it may be a configuration parameter that affects adapter initialization or resource allocation.

## Callback Behaviour

### `FUN_140008f34` – BAR discovery & mapping (Ghidra Analysis)

**Location**: `rcbottom.sys`

**Entry**: `(param_1, param_2, param_3)` where:
* `param_1`: device extension
* `param_2`: service table
* `param_3`: HAL context

**Stack Layout** (from Ghidra):
* `local_res18`: Cached param_3 (HAL context) at offset +0x18
* `local_res10`: Parameter 2 (service table) at offset +0x10
* `local_res8`: Parameter 1 (device extension) at offset +0x8
* `local_38`: Capability data buffer (offset -0x38)
* `local_34`: 16-bit capability descriptor (offset -0x34)
* `local_30`: Capability flag 1 (offset -0x30, checked for bit 0x8000)
* `local_2c`: Capability flag 2 (offset -0x2c, checked for bit 1)
* `local_48`: Loop counter/index (offset -0x48)

**Key Code Path**:
```c
// Service slot +0x650: Returns adapter context
// Parameters: (serviceCtx, devExt, PTR_DAT_140014090)
// PTR_DAT_140014090 points to DAT_140014078 = 0x28 (40 decimal)
// This constant is referenced in 24+ functions and may be a configuration parameter
lVar6 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980, param_1, PTR_DAT_140014090);

// MmMapIoSpace is called at offset 1400090cb to map memory BARs
// Signature: MmMapIoSpace(PhysicalAddress, Length, CacheType)
// Returns: Virtual address (stored at devExt+0x10)

// CRITICAL: Check if queue_state == 2 (triggers special capability parsing)
if (*(int *)(lVar6 + 0x16068) == 2) {
    // Use descriptor accessor at devExt+0x1C2D0 to read capability data
    (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0), 0, local_38, 0x34, 1);
    
    // Read capability bits using accessor in loop
    // local_30 & 0x8000 - capability flag
    // local_2c & 1 - another capability flag
    // Loop continues while uVar10 < 10 and reads opcodes 0x11, 0x05
    
    // Set BAR type based on capabilities
    if (((local_30 & 0x8000) != 0) && ((local_2c & 1) == 0)) {
        uVar2 = 1;  // Set BAR type flag
    }
}
*(undefined1 *)(lVar6 + 0xb5) = uVar2;  // Store BAR type at devExt+0xb5
```

**BAR Mapping Logic** (from Ghidra):
* Retrieves the miniport service table (`*(DAT_140014958 + 0x650)`).
* Iterates adapter resources via service calls @`+0x980` (count) and `+0x988` (enumerate).
* For each resource:
  ```c
  pcVar7 = (char *)(**(code **)(DAT_140014958 + 0x988))(DAT_140014980, param_3, uVar10);
  
  if (*pcVar7 == '\x02') {
      // Type 2 - I/O port or special resource
      FUN_1400079a4(lVar6, uVar8, pcVar7);  // Call port registration
  }
  else if ((*pcVar7 == '\x03') && (*(longlong *)(lVar6 + 0x10) == 0)) {
      // Type 3 - Memory BAR (only map first one)
      uVar8 = *(undefined8 *)(pcVar7 + 4);      // Physical address at offset +4
      uVar1 = *(undefined4 *)(pcVar7 + 0xc);    // Length at offset +0xC
      uVar8 = MmMapIoSpace(uVar8, uVar1, 0);    // Map to kernel virtual address
      *(undefined8 *)(lVar6 + 0x10) = uVar8;    // Store virtual address at devExt+0x10
      *(undefined4 *)(lVar6 + 0x18) = uVar1;   // Store length at devExt+0x18
  }
  ```
* After mapping, it loops through the BARs and either maps additional windows or hands them off to helper routines.
* Invokes `FUN_140008a48` / `FUN_140008bc0` / `FUN_140008d88` / `FUN_14000924c` to configure queues, spinlocks, and doorbells.

**Key Insights**:
1. **Queue State Check**: `devExt+0x16068 == 2` triggers special capability parsing using descriptor accessor
2. **Descriptor Accessor**: Uses function pointer at `devExt+0x1C2D0` to read capability data (opcodes 0x11, 0x05)
3. **BAR Type Detection**: Sets `devExt+0xb5` based on capability bits (`local_30 & 0x8000` and `local_2c & 1`)
4. **Memory BAR**: Only maps first memory BAR (type 3) to `devExt+0x10` - checks `devExt+0x10 == 0` before mapping

### `FUN_140008a48` (Queue initialization - called from `FUN_140008638`)

**Location**: `rcbottom.sys`

**Purpose**: Initializes queues and completion register contexts for a BAR

**Parameters**: `param_1` (adapter handle)

**Logic** (from decompiled code):
* Gets device extension via StorPort service `+0x650`
* **Calls service slot `+0x9D8` twice** (completion register programming):
  * First call: `(serviceCtx, 0, devExt + 0x16288)` - programs completion register context at offset 0x16288
  * Second call: `(serviceCtx, 0, devExt + 0x1c2e8)` - programs completion register context at offset 0x1c2e8
* Initializes spinlocks for all ports (iterates through `devExt+0xb0` count)
* Calls `FUN_140007978()` which calls service slot `+0x9D8` with global address `&DAT_140216880`
* Calls callback at `devExt+0x16110` (which is `FUN_1400021d4` - spinlock callback)
* Uses StorPort service `+0x9f8` to write `0xffffffffffe17b80` to MMIO at `devExt+0x16010`
* Clears `devExt+0x16054` to 0
* Calls `FUN_1400093c4` to check/clear queue state
* If `FUN_1400093c4` returns 1, calls `FUN_14000273c` to log error

**Key Offsets**:
* `devExt + 0x16288` - Completion register context (first call to +0x9D8)
* `devExt + 0x1c2e8` - Completion register context (second call to +0x9D8)
* `devExt + 0x16010` - MMIO register address (written via service +0x9f8)
* `devExt + 0x16050` - Used as index into table for `FUN_1400093c4`

**NOTE**: This function calls service slot `+0x9D8` with different offsets than `FUN_1400021d4`, suggesting multiple completion register contexts per adapter.

### `FUN_140007978` (Global completion register programming)

**Location**: `rcbottom.sys`

**Purpose**: Programs completion registers using a global context

**Parameters**: None

**Logic** (from decompiled code):
* Resets `DAT_140014284` to 0
* **Calls service slot `+0x9D8` with global address**: `(serviceCtx, 0, &DAT_140216880)`
* This is a global completion register context, not per-device

**NOTE**: This suggests there's a global completion register context at `DAT_140216880` that needs to be programmed separately from per-device contexts.

### `FUN_1400093c4` (Queue state table lookup)

**Location**: `rcbottom.sys`

**Purpose**: Checks and clears a value in a global table

**Parameters**: `param_1` (devExt)

**Logic** (from decompiled code):
* Calculates index: `index = devExt+0x16050 * 0x1c878`
* Reads value from `DAT_140031488[index]`
* Clears `DAT_140031488[index]` to 0
* Returns the old value

**Returns**: 0 if queue state was already clear, 1 if it was set (indicating error condition)

**NOTE**: This appears to be a global table tracking queue initialization state. Returning 1 indicates a previous initialization error.

### `FUN_14000273c` (Error logging)

**Location**: `rcbottom.sys`

**Purpose**: Logs error when AHCI controller GHC register has invalid value

**Parameters**: `param_1` (device extension)

**Logic** (from decompiled code):
* Allocates error log entry via `IoAllocateErrorLogEntry`
* Sets error code `0xc004000b`
* Sets error type `0x300001`
* Copies error message: `"  AHCI controller GHC register invalid value"`
* Writes error log entry

**NOTE**: This is diagnostic/error logging only, not critical for implementation.

### `FUN_140008bc0` (Descriptor/WMI registration and cleanup - called from `FUN_140008638`)

**Location**: `rcbottom.sys`

**Purpose**: Registers descriptors/WMI and releases completion register contexts (cleanup/teardown)

**Parameters**: `param_1` (adapter handle)

**Logic** (from decompiled code):
* Gets device extension via StorPort service `+0x650`
* Copies descriptor data from global table `DAT_1402164b8`:
  * Reads count from `DAT_1402164b0`
  * Copies `0x1c878` bytes per entry from `DAT_1402164b8+0x18` to `DAT_140014c40`
  * Uses `FUN_140011140` (memcpy) for copying
* Uses StorPort service `+0xa00` to enable something on MMIO at `devExt+0x16010`
* Calls callback at `devExt+0x16118` (status polling cleanup - `FUN_140003f7c`)
* If `devExt+0x1c2dc != 0`:
  * Uses StorPort service `+0x6d8` with `devExt+0x16020`
  * Calls `FUN_1400043e0` to iterate/validate queues
  * If `devExt+0x16050 == 1`:
    * Gets adapter context via StorPort service `+0x650`
    * Iterates through adapter contexts and resets them
* **Releases completion register contexts**:
  * Releases spinlock for `devExt+0x16288` via StorPort service `+0x680`
  * Clears `devExt+0x16288` to 0
  * Releases spinlock for `devExt+0x1c2e8` via StorPort service `+0x680`
  * Clears `devExt+0x1c2e8` to 0
* Clears `devExt+0x1c2dc` to 0
* Returns value from StorPort service `+0x6d8` call (if `devExt+0x1c2dc` was set)

**Key Offsets**:
* `devExt + 0x16288` - Completion register context (released here, initialized in `FUN_140008a48`)
* `devExt + 0x1c2e8` - Completion register context (released here, initialized in `FUN_140008a48`)
* `devExt + 0x16010` - MMIO register address (used with service +0xa00)
* `devExt + 0x16020` - **Adapter/controller handle** (primary adapter context pointer, used with services `+0x650`, `+0x6d8`, `+0x680`)
* `devExt + 0x1c2dc` - Flag indicating if cleanup needed

**WMI/Descriptor Registration** (from earlier Ghidra analysis):
* Stores config data into a global at `DAT_1402164B0`
* Retrieves a port configuration block using service `+0x3F0` (2 times)
* Iterates CLSIDs (`"CC_010802"` etc.) to identify controller type
* Uses `wcsncmp` to match controller descriptors
* Checks feature bitfield `DAT_1400146B0` and descriptor mask at `entry+0x2C`
* Registers WMI helpers and work items based on feature flags
* See detailed WMI registration logic in section below

**NOTE**: This is the cleanup/teardown function that releases the completion register contexts initialized by `FUN_140008a48`. It also handles WMI/descriptor registration during initialization.

### `FUN_1400043e0` (Queue validation/iteration)

**Location**: `rcbottom.sys`

**Purpose**: Iterates through and validates queue configuration

**Parameters**: `param_1` (devExt)

**Logic** (from decompiled code):
* Iterates through queues (up to 0x30 = 48 queues)
* For each queue:
  * Checks if queue is active via bitmask at `devExt+0x158e0`
  * If active, reads queue depth from `devExt+0x158fd`
  * Validates queue configuration
* Returns when all queues checked

**NOTE**: This appears to be a validation/check function called during cleanup to ensure queues are properly configured before teardown.

### `FUN_140011140` (memcpy/memmove implementation)

**Location**: `rcbottom.sys`

**Purpose**: Standard memory copy function with overlap handling

**Parameters**: `param_1` (destination), `param_2` (source), `param_3` (size)

**Logic**:
* Handles overlapping buffers (memmove semantics)
* Optimized for different sizes:
  * < 8 bytes: byte-by-byte copy
  * < 0x11 bytes: 8-byte copy
  * < 0x21 bytes: 16-byte copy
  * >= 0x21 bytes: optimized 64-byte chunks with alignment handling
* Handles forward and backward copying for overlapping regions

**NOTE**: This is a standard C library memcpy/memmove implementation, not critical for driver implementation.

### `FUN_140008d88`
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

### `FUN_140005ff4` – Adapter initialization and device enumeration

* **Purpose**: Main adapter initialization function that sets up adapter contexts, parses device information, and initializes firmware capabilities.
* **Stack layout**: Large stack frame with multiple local buffers (`local_c38[512]`, `local_838[2048]`, `local_cb8[80]`).
* **Initialization sequence**:
  1. **Device information retrieval**: 
     * Calls service `+0x3f0` with queue index 1 to read device information into `local_c38` (512 bytes)
     * Calls service `+0x3f0` with queue index 2 to read device information into `local_838` (2048 bytes)
     * Searches for "CC_010802" string in device information
  2. **Firmware version parsing**:
     * Calls service `+0x3f0` with queue index 10 to read firmware version string from `DAT_1400142a0`
     * Parses version string to extract major, minor, and patch version numbers
  3. **Adapter context creation**:
     * Calls service `+0x258` (600 decimal) to create adapter context, stores result in `local_d78`
     * Calls service `+0x650` to get adapter extension pointer using `PTR_DAT_140014090`
     * Stores adapter handle in global array `DAT_140216780` at index `DAT_140014294`
     * Sets adapter fields: `puVar5[0] = 0`, `puVar5[8] = local_d78`, `puVar5[10] = uVar6` (from service `+0x108`)
     * Combines parsed version numbers: `puVar5[0x5818] = (iVar4 << 8 | uVar15) << 8 | uVar10`
  4. **Firmware capability parsing**:
     * Calls `FUN_140007d40(local_c38, puVar5, uVar17)` to parse firmware capabilities and install callbacks
  5. **Adapter linkage setup**:
     * If `DAT_140014288 == 0` (first adapter):
       * Calls service `+0xa8` to set adapter pointers at `puVar5+0x580a` and `puVar5+0x580c`
       * Calls `FUN_1400067fc` to complete adapter setup
     * Else (subsequent adapters):
       * Calls service `+0x6e8` to get previous adapter context
       * Copies `devExt+0x16020` and `devExt+0x16030` from previous adapter to current adapter
       * Links adapters through global adapter list structure
  6. **WMI/descriptor registration**:
     * Calls service `+0x418` with `DAT_140012208` to initialize descriptor accessor at `puVar5+0x5824`
     * Sets up WMI configuration structure
     * Calls service `+0x298` for WMI registration
* **Key fields set**:
  * `devExt+0x16028` and `devExt+0x16030`: Set from previous adapter (if not first)
  * `devExt+0x16018`: Set during `FUN_1400067fc` call
  * `devExt+0x16020`: Set from `devExt+0x16018` in `FUN_1400067fc`
  * `devExt+0x16050`: Set during adapter linkage
  * `devExt+0x1c7b1` and `devExt+0x1c7b2`: Set based on device capabilities
* **Call sites**: Called from adapter enumeration/initialization path

### `FUN_1400067fc` / `FUN_14000681f` – Adapter object & WMI registration helper

* **Purpose**: Completes adapter setup after initial context creation, sets up WMI registration, and establishes adapter linkage.
* **Initialization sequence**:
  1. **Adapter context setup**:
     * Calls service `+0x650` to get adapter extension pointer
     * Calls service `+0x690` to get adapter handle
     * Calls service `+0x258` with adapter handle, stores result in `devExt+0x16018`
     * **Sets `devExt+0x16020` from `devExt+0x16018`**: `*(longlong *)(lVar2 + 0x16020) = *plVar1` (where `plVar1 = lVar2 + 0x16018`)
     * If `DAT_140014288 == 0`, sets global `DAT_140014288 = devExt+0x16020` (first adapter)
     * Increments `DAT_140014290` (adapter count)
  2. **Adapter linkage**:
     * Gets adapter list context via service `+0x650` with `PTR_DAT_140014040`
     * Adds current adapter to list at `lVar3+0x18`
     * Sets `devExt+0x16050 = 0`
  3. **WMI registration**:
     * Calls service `+0x298` to create WMI object
     * Calls service `+0x2a0` with WMI configuration
     * Calls `FUN_14000a430` (WMI GUID binder)
     * Calls service `+0x428` to register adapter
     * Calls service `+0xe8` with configuration
     * Rings doorbells 1, 2, 3 via service `+0x188`
* **Net result**: Populates `devExt+0x16020` (adapter handle), `devExt+0x16018` (adapter context), and `devExt+0x16050` (adapter index). Seeds StorPort with WMI dispatch tables and establishes adapter linkage.

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

### `FUN_14000924c` (See "Queue Activation Path" section below for detailed Ghidra analysis)

* Marks the adapter active (`devExt+0x16054 = 1`, `devExt+0x1607C` flags).
* Issues four doorbell writes by calling `service(+0x188)` with indices 1…4.
* If `devExt+0x16068 == 1`, also calls `FUN_140001ED8` (legacy path).
* `FUN_14000924c` sequence (queue activation):
  * Begins by calling service slot `+0x650` with `(StorPortRegister?)` then sets `devExt+0x16054 = 1`. Immediately loads `ECX=0x1388` (decimal 5000) and calls `KeStallExecutionProcessor`, so the legacy path is separated by a 5 µs stall.
  * If `devExt+0x16068 == 1` it calls `FUN_140001ED8` and then enforces a second stall with `ECX=0x61A8` (decimal 25000).
  * If `devExt+0x1607C` was zero it calls `FUN_14000A564` (WMI/descriptor binder) before marking `devExt+0x1C2DC = 1`.
  * Rings the firmware doorbells by loading `devExt+0x16020` into `RDX` four consecutive times and invoking service `+0x188` (indices 1..4 in the Windows driver). Each call is separated only by service latency; no explicit delay between them in this block.
  * Returns immediately afterward.

## Interrupt & Completion Notes

### `FUN_14000d29c` (ISR resource cleanup - called from `FUN_14000d2b8`)

**Location**: `rcraid.sys`

**Purpose**: Cleans up ISR/interrupt resources (DMA buffers and spinlocks)

**Parameters**: `param_1` (device extension or SRB context)

**Logic** (from decompiled code and listing):
* Checks if `param_1 + 0x698 != 0` (memory pool pointer)
* If non-zero:
  * Frees memory at `param_1 + 0x698` with tag `0x72635041` (`ExFreePoolWithTag`)
    * Tag `0x72635041` = `'APCr'` (ASCII reversed) - likely "RC AP" for AMD RAID
  * Releases primary spinlock at `param_1 + 0x6b8` via StorPort service `+0x680`
  * Loops through array of spinlocks at `param_1 + 0x6c0`:
    * Count is at `param_1 + 0x678`
    * Releases each spinlock via StorPort service `+0x680`
  * Frees spinlock array at `param_1 + 0x6c0` with tag `0x72635041`
* Clears all pointers to 0:
  * `param_1 + 0x698` = 0
  * `param_1 + 0x670` = 0
  * `param_1 + 0x678` = 0

**Key Offsets**:
* `param_1 + 0x698` - Memory pool pointer (DMA buffer for queue/completion data)
* `param_1 + 0x6b8` - Primary queue spinlock handle (single spinlock)
* `param_1 + 0x6c0` - Array of per-queue spinlock handles
* `param_1 + 0x678` - Count of spinlocks in array at `+0x6c0`
* `param_1 + 0x670` - Scratch pointer (cleared but not actively used)

**StorPort Services Used**:
* `+0x680` - Release spinlock (called multiple times for each spinlock)

**NOTE**: This is the cleanup function for ISR/interrupt resources. It frees DMA-allocated memory pools and releases all spinlocks allocated for interrupt handling. Called during ISR teardown/shutdown.

### `FUN_14000d2b8` – Interrupt service routine

* Writes entry/exit breadcrumbs via ETW helpers (`call *0x4E61(%rip)` / `*0x4E01(%rip)`) with tag `0x72635041`.
* Pulls the primary queue handle from `devExt+0x6B8` and iterates the array at `devExt+0x6C0`, invoking StorPort service `+0x680` for each entry (completion pump).
* Uses `devExt+0x678` as the pending count then clears `devExt+0x698` / `+0x670` / `+0x678` before returning.

### `FUN_14000c2fc` (Descriptor lookup - NVMe command slot allocation)

**Location**: `rcraid.sys`

**Purpose**: Allocates a free command descriptor slot from a queue and initializes it

**Parameters**: 
* `param_1` (queue handle structure pointer)
* `param_2` (callback function pointer for completion)

**Logic** (from decompiled code):
* Checks if queue has available slots: `(producer + 1) % queue_size != consumer`
* If queue has space:
  * Searches for free slot in descriptor array:
    * Starts at `next_slot_index` (offset +0x4)
    * Iterates up to `queue_depth` (offset +0x2) slots
    * Checks if slot is free: `descriptor[slot_index][+0x28] == 0`
  * When free slot found:
    * Zeros slot descriptor (64 bytes at offset +0x30) using `FUN_140011400` (memset)
    * Stores callback at `descriptor[slot_index][+0x28] = param_2`
    * Sets slot index: `descriptor[slot_index][+0x20] = slot_index`
    * Sets queue index: `descriptor[slot_index][+0x22] = producer_index`
    * Updates `next_slot_index` to found slot
    * Increments producer index: `producer = (producer + 1) % queue_size`
    * Returns pointer to descriptor slot
* Returns `0` if no free slot available

**Queue Structure** (param_1):
* `+0x00`: queue handle pointer
* `+0x02`: queue depth (ushort) - maximum number of slots
* `+0x04`: next slot index (ushort) - where to start searching
* `+0x06`: producer index (ushort) - next slot to produce
* `+0x08`: consumer index (ushort) - next slot to consume
* `+0x0a`: queue size (ushort) - total queue size
* `+0x0c`: queue base address (qword) - MMIO queue base
* `+0x48`: descriptor array base (qword) - array of 0x78-byte descriptor structures

**Descriptor Slot Structure** (0x78 bytes):
* `+0x20`: slot index (ushort) - slot number in queue
* `+0x22`: queue index (ushort) - producer index when allocated
* `+0x28`: callback function pointer (qword) - completion callback
* `+0x30`: command type (byte) - NVMe command opcode
* `+0x32`: slot index copy (ushort) - duplicate of +0x20
* `+0x34`: context/parameter (dword) - command context
* `+0x38`: metadata (qword) - command metadata
* `+0x40-0x4f`: command data (16 bytes) - NVMe command DWORDs
* `+0x48-0x4f`: command data (8 bytes) - NVMe command DWORDs
* `+0x50-0x5f`: command data (16 bytes) - NVMe command DWORDs
* `+0x58-0x5f`: command data (8 bytes) - NVMe command DWORDs
* `+0x60-0x6f`: command data (16 bytes) - NVMe command DWORDs
* `+0x68-0x6f`: command data (8 bytes) - NVMe command DWORDs
* `+0x70`: completion callback (qword) - SRB completion callback

**Call Pattern** (used by 20+ command submission functions):
1. Call `FUN_14000c2fc(queue_handle, callback)` to allocate descriptor slot
2. Fill in command descriptor fields:
   * Set `descriptor[+0x30]` = command type (opcode)
   * Set `descriptor[+0x34]` = context/parameter
   * Set `descriptor[+0x48-0x68]` = command data (NVMe command DWORDs)
   * Set `descriptor[+0x70]` = completion callback
   * Copy `descriptor[+0x32] = descriptor[+0x20]` (slot index)
3. Copy descriptor to MMIO queue:
   * Calculate slot offset: `slot_offset = descriptor[+0x22] * 0x40`
   * Copy 64 bytes from `descriptor[+0x30]` to `queue_base[+0xc] + slot_offset`
   * Copy in 16-byte chunks: +0x0, +0x10, +0x20, +0x30
4. Update producer index:
   * `queue_base[+0x1000 + queue_id * 8] = (producer + 1) % queue_size`

**NOTE**: This is the core function for NVMe command submission in `rcraid.sys`. All command submission functions use this pattern to allocate and submit commands to the hardware queue.

### `FUN_14000c900` (SRB Completion/Error Handler)

**Location**: `rcraid.sys`

**Purpose**: Universal completion/error handler for SRB requests. Called when commands complete, fail, or need error handling.

**Parameters**:
* `param_1` (longlong): SRB pointer
* `param_2` (longlong): Completion callback pointer (or 0 for direct completion)
* `param_3` (char): Status byte (0 = success, 2 = error)
* `param_4` (ushort): Status word (error code, e.g., `0x2600`, `0x8407`, `0x2000`, `0xc00`)

**Logic**:
1. **DMA Cleanup**: If `param_1[+0x58]` (scatter-gather list handle) is non-zero:
   * Calls StorPort service `+0x338` to get scatter-gather list metadata
   * Calls StorPort service `+0x680` to free the scatter-gather list
   * Zeroes `param_1[+0x58]`

2. **SRB Status Field Updates**:
   * `param_1[+0x100] = param_1[+0x13c]` (original status)
   * `param_1[+0x110] = param_3` (status byte: 0=success, 2=error)
   * `param_1[+0x10c] = 0` (clear secondary status)
   * `param_1[+0x112] = 0` (clear command type)
   
3. **Status Word Handling**:
   * If `param_3 == 0` (success):
     * `param_1[+0x10e] = 0` (no error code)
     * `param_1[+0x104] = param_1[+0x140]` (restore original information)
   * Else (error):
     * `param_1[+0x10e] = param_4 << 8 | param_4 >> 8` (byte-swapped status word)
     * `param_1[+0x104] = 0` (clear information)

4. **Completion**:
   * If `param_2 == 0`: Calls `FUN_1400097ac(param_1)` for direct completion
   * Else: Calls StorPort service `+0x838(param_2, 0)` for callback completion

**Called by**: Many command submission functions when errors occur:
* `FUN_14000cf38`, `FUN_14000d3f4`, `FUN_14000e2c8`, `FUN_14000ea34` (when scatter-gather list is NULL)
* `FUN_14000ec64`, `FUN_14000ed2c`, `FUN_14000f838`, `FUN_14000fa2c` (error paths)
* Completion callbacks: `FUN_14000e1ec`, `FUN_14001005c`, `FUN_140010184`

**Error Codes**:
* `0x2600`: General error
* `0x8407`: Invalid parameter
* `0x2000`: Command not supported
* `0xc00`: Invalid command

**NOTE**: This function is called by many command submission functions when descriptor allocation fails or command cannot be submitted.

### NVMe Command Submission Functions (All use `FUN_14000c2fc`)

**Location**: `rcraid.sys`

All of these functions follow the same pattern:
1. Call `FUN_14000c2fc` to allocate descriptor slot
2. Fill in command descriptor fields
3. Copy descriptor to MMIO queue
4. Update producer index

**Functions identified** (20+ functions):

* **`FUN_14000c0bc`** - NVMe command submission (cmd_type=8)
  * Called from primary queue dispatcher
  * Handles SRB completion callback

* **`FUN_14000c718`** - Command submission (cmd_type=2 or 0xc)
  * Handles scatter-gather commands
  * Sets command size based on parameter (0x40 or 0x200)

* **`FUN_14000cb4c`** - Command submission (cmd_type=5)
  * Handles multi-command sequences

* **`FUN_14000cc28`** - State machine command submission (cmd_type=4 or 1)
  * Complex state machine with locking
  * Handles command sequencing and completion

* **`FUN_14000cd50`** - Command helper (cmd_type=1)
  * Called from `FUN_14000c0bc` when param_3==0
  * Handles command sequencing

* **`FUN_14000ce5c`** - Iterates and submits commands (cmd_type=0)
  * **Parameters**: `param_1` (queue handle), `param_2` (callback)
  * **Logic**: Loops through command array `param_1[6]` times
    * Allocates descriptor slot for each iteration
    * Sets `cmd_type=0` and `descriptor[+0x58] = loop_index + 1`
    * Submits multiple commands in sequence
  * **Used by**: `FUN_14000c0bc` when `param_3==0`

* **`FUN_14000cf38`** - Command submission (cmd_type=9)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (command pointer), `param_4` (completion)
  * **Logic**: 
    * Uses queue at `devExt+0x15948`
    * If `param_3==NULL`, calls `FUN_14000c900` for error handling
    * Sets context from `devExt+0x15ce8`
    * Sets command data from `param_3` and `param_2[+0x61/0x62]`
  * **Used by**: Primary queue dispatcher for SRB-based commands

* **`FUN_14000d190`** - Command submission (cmd_type=0)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion)
  * **Logic**: 
    * Uses queue at `devExt+0x15948`
    * Sets `cmd_type=0` and context from `devExt+0x15ce8`
    * Sets SRB tracking fields: `param_2[+0xc0]` and `param_2[+0xc4]`
  * **Returns**: `true` if descriptor allocated, `false` otherwise

* **`FUN_14000d350`** - Command submission (cmd_type=10)
  * **Parameters**: `param_1` (queue handle)
  * **Logic**: 
    * Sets `cmd_type=10` and `descriptor[+0x16]=0xd`
    * Uses callback `FUN_14000eef8`
    * Submits queue management command

* **`FUN_14000d3f4`** - Scatter-gather command (cmd_type=2)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather pointer), `param_4` (completion)
  * **Logic**: 
    * Uses queue at `devExt+0x15940`
    * **Detects scatter-gather size from `param_2[+0x61]`:**
      * `== 1`: 0x40 bytes (64 bytes)
      * `== 2 or 3`: 0x200 bytes (512 bytes)
    * Sets `descriptor[+0x16]` based on size and type
    * If type==2, sets `descriptor[+0xd] = -1`
    * If `param_3==NULL`, calls `FUN_14000c900` for error handling

* **`FUN_14000d66c`** - Queue management command (cmd_type=6)
  * **Parameters**: `param_1` (queue handle)
  * **Logic**: 
    * **Always submits** (no null check - assumes slot available)
    * Sets `cmd_type=6` and `descriptor[+6]=6`
    * Uses callback `FUN_14000d714`
    * Sets `descriptor[+0x34]=0`

* **`FUN_14000d974`** - Queue management command (cmd_type=6)
  * **Parameters**: `param_1` (queue handle)
  * **Logic**: 
    * **Checks for null descriptor** before submitting
    * Sets `cmd_type=6` and `descriptor[+6]=6`
    * Uses callback `FUN_14000d714`
    * Sets `descriptor[+0x16]=1` and `descriptor[+0x34]=0`

* **`FUN_14000da20`** - Queue management command (cmd_type=6)
  * **Parameters**: `param_1` (queue handle), `param_2` (dword parameter)
  * **Logic**: 
    * **Takes `param_2` as command parameter**
    * Sets `cmd_type=6` and `descriptor[+0xc]=6`
    * Sets `descriptor[+0xd]=param_2` and `descriptor[+0x16]=0`
    * Uses callback `FUN_14000d714`

* **`FUN_14000dae4`** - Complex state machine (cmd_type=1 or 5)
  * **Parameters**: `param_1` (queue handle), `param_2` (unused), `param_3` (completion descriptor), `param_4` (SRB)
  * **Logic**: 
    * **Checks `param_4[+0xc] < 0x20000`** (size check - early exit if too large)
    * **Path 1**: If `param_3[+0x30] == 0x01`:
      * Increments `param_1[6]` counter
      * If counter reaches `param_1[+0x3dc]`, sets flag `param_1[+0x73] |= 0x10`
      * Submits command with `cmd_type=1`
      * Uses data from `param_1[(counter+1)][+0x28]` and `param_1[(counter+1)][+10]`
      * Complex sequencing with `param_1[6]` indexing into command array
    * **Path 2**: If `param_3[+0x30] == 0x05`:
      * Sets flag `param_1[+0x73] |= 0x20`
      * Increments counter
      * If counter reaches limit (`param_1[+0x3dc]`), resets to 0 and submits `cmd_type=1` with wrap-around
      * Otherwise submits `cmd_type=5` with different parameters
      * Checks `param_1[+0x7a] < 2` condition for special handling
    * **Completion**: When `param_1[+0x73] & 0x37 == 0x37` (all flags set):
      * Loops calling `FUN_14000e4d0` until it returns 0 (submit helper commands)
      * Sets `param_1[+0x78] = 1` (completion flag)
      * Calls `FUN_14000c1e4(param_1)` to configure and submit final command
  * **State Machine Flags**:
    * `param_1[+0x73]` bits: 0x10 (path 1 complete), 0x20 (path 2 complete), 0x37 (all complete)
    * `param_1[+0x78]`: Completion flag (set to 1 when done)
  * **NOTE**: This is one of the most complex command submission functions with state machine logic. It handles multi-phase command sequences and coordinates multiple command submissions before finalizing.

* **`FUN_14000e2c8`** - Queue type selection command (cmd_type varies)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion)
  * **Logic**: 
    * **Selects queue based on `param_2[+0x60]`:**
      * `== 0x01`: Uses primary queue at `devExt+0x15940`
      * `== 0x02`: Uses secondary queue at `devExt+0x15948[queue_index]`
      * Otherwise: Returns 0
    * Sets `cmd_type` from `param_2[+0x61]`
    * **Handles scatter-gather list based on `param_2[+0x64]` bits:**
      * Bit 2 (0x4): Uses `param_2[+0x6c]` for `descriptor[+0x58]`
      * Bit 3 (0x8): Uses `param_2[+0x70]` for `descriptor[+0x5c]`
      * Bit 4 (0x10): Uses `param_2[+0x74]` for `descriptor[+0x60]`
      * Bit 5 (0x20): Uses `param_2[+0x78]` for `descriptor[+0x64]`
      * Bit 6 (0x40): Uses `param_2[+0x7c]` for `descriptor[+0x68]`
      * Bit 7 (0x80): Uses `param_2[+0x80]` for `descriptor[+0x6c]`
    * Calls `FUN_14000c9e4` before submitting
    * If `FUN_14000c9e4` fails, calls `FUN_14000c900` for error handling
  * **Returns**: 1 if submitted, 0 if failed

* **`FUN_14000e4d0`** - Command helper with limits (cmd_type=0xc)
  * **Parameters**: `param_1` (queue handle)
  * **Logic**: 
    * **Checks queue depth limit**: `param_1[+0x39c] < min(2, param_1[+0x351])`
    * Only submits if limit not reached
    * Increments `param_1[+0x39c]` counter
    * Sets `cmd_type=0xc`
    * Uses callback `FUN_14000c718`
  * **Returns**: 1 if submitted, 0 if limit reached
  * **Used by**: `FUN_14000dae4` in completion loop

* **`FUN_14000ea34`** - Queue rotation command (cmd_type=1 or 2)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion)
  * **Logic**: 
    * **Uses queue at `devExt+0x15948[queue_index]`** where `queue_index = devExt+0x15968`
    * **Rotates queue when counter reaches threshold:**
      * Uses `DAT_140014160` or `DAT_140014164` as threshold (based on `param_2[+0x188]`)
      * Increments `devExt+0x1596c` counter
      * When counter >= threshold, resets to 0 and increments `queue_index`
      * `queue_index = (queue_index + 1) % devExt[+0x15d1c]`
    * Sets `cmd_type=1` or `cmd_type=2` based on `param_2[+0x154] & 4`
    * Calls `FUN_14000c9e4` before submitting
    * If `FUN_14000c9e4` fails, calls `FUN_14000c900` for error handling
  * **Returns**: 1 if submitted, 0 if failed
  * **NOTE**: This implements load balancing across multiple queues

* **`FUN_14000ee30`** - Command with parameters (cmd_type=9)
  * **Parameters**: `param_1` (queue handle), `param_2` (dword), `param_3` (dword), `param_4` (callback)
  * **Logic**: 
    * Sets `cmd_type=9`
    * Sets `descriptor[+0x16]=param_2` and `descriptor[+0x17]=param_3`
    * Uses `param_4` as callback
  * **Returns**: Queue handle with high byte set if submitted, 0 otherwise

* **`FUN_14000ef80`** - Command with flags (cmd_type=9)
  * **Parameters**: `param_1` (queue handle), `param_2` (byte flags)
  * **Logic**: 
    * Sets `cmd_type=9` and `descriptor[+0xc]=9`
    * Sets `descriptor[+0x16]=0xd` and `descriptor[+0x17]=param_2`
    * **If `param_2 & 1` (bit 0 set):**
      * Sets `descriptor[+0x18]` through `descriptor[+0x1b]` from `param_1[+0xcf]` through `param_1[+0xd1]`
    * Uses callback `FUN_14000eef8`

* **`FUN_14000f06c`** - Command with context (cmd_type=9)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (completion), `param_4` (dword)
  * **Logic**: 
    * Uses queue at `devExt+0x15940`
    * Sets `cmd_type=9` and `descriptor[+0xc]=9`
    * Sets `descriptor[+0x16]=2`
    * Sets `descriptor[+0x17]` from `param_4` or `devExt[+0x15c92]` if `param_4==0`
    * Sets `descriptor[+6]` and `descriptor[+7]` to `param_2` (SRB pointer)
    * Sets SRB tracking: `param_2[+0xc0]` and `param_2[+0xc4]`
    * Uses callback `FUN_14000eef8`

* **`FUN_14000fe44`** - Command with context (cmd_type=4)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion)
  * **Logic**: 
    * Uses queue at `devExt+0x15948`
    * Sets `cmd_type=4` and context from `devExt+0x15ce8`
    * Sets command data from `param_2[+0x64]` and `param_2[+0x6c]`
    * Sets SRB tracking: `param_2[+0xc0]` and `param_2[+0xc4]`
    * Uses callback `FUN_14000e1ec`

* **`FUN_14000ff50`** - Special command (cmd_type=0x11)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion)
  * **Logic**: 
    * Uses queue at `devExt+0x15940`
    * Sets `cmd_type=0x11`
    * Sets `descriptor[+0x58]=0` and `descriptor[+0x5c]=0`
    * Calls `FUN_14000c9e4` before submitting
    * Sets SRB tracking: `param_2[+0xc0]` and `param_2[+0xc4]`
    * Uses callback `FUN_14001005c`
  * **Returns**: 1 if submitted, 0 if failed

**NOTE**: All these functions use the same descriptor submission pattern documented in `FUN_14000c2fc`. The pattern is:
1. Allocate descriptor slot
2. Fill command fields
3. Copy to MMIO queue (64 bytes at offset `slot_index * 0x40`)
4. Update producer index at `queue_base + 0x1000 + queue_id * 8`

## Scatter-Gather List Helper Functions (rcraid.sys)

### `FUN_14000c9e4` - Scatter-Gather List Builder (Pre-Submission)

**Location**: Analyzed from `rcbottom.sys`, but used by `rcraid.sys` functions
**Note**: Should verify if this function exists in `rcraid.sys` at the same address or if it's exported/imported between modules

**Purpose**: Builds scatter-gather list for DMA data transfer before command submission. Called by command submission functions to set up data buffers for read/write operations.

**Parameters**:
* `param_1` (longlong): Queue handle pointer (typically `devExt+0x15940`)
* `param_2` (longlong): SRB pointer
* `param_3` (longlong*): Scatter-gather list pointer (can be NULL for commands without data transfer)
* `param_4` (longlong): Command descriptor pointer (allocated by `FUN_14000c2fc`)

**Logic**:
1. **Transfer Length Calculation**:
   * Gets transfer length from `param_2[+0x140]` (SRB information length)
   * If zero, uses `param_2[+0x188]` (SRB data transfer length)
   * If length is 0, returns `true` (no data transfer needed)

2. **Scatter-Gather List Processing** (if `param_3 != NULL`):
   * **First SGL entry**: 
     * Reads physical address from `param_3[0]` (first SGL entry pointer)
     * Reads length from `param_3[0][+0x10]` (SGL entry length DWORD at offset +0x10)
     * Sets `param_4[+0x48] = param_3[0][0]` (first physical address)
     * Calculates alignment: `uVar1 = 0x1000 - (address & 0xfff)` (page alignment)
     * Processes first chunk: `uVar2 = min(uVar1, uVar8)` (aligned portion)
     * Remaining: `uVar8 -= uVar2`, `uVar7 -= uVar2` (update remaining length)

3. **Multi-Page Processing** (if transfer length >= 0x1001 bytes):
   * **Scatter-gather descriptor array**: 
     * Accesses descriptor array at `param_1[+0x8 + queue_index*8][+0x38]` or `[+0x40]`
     * Each descriptor is 8 bytes (0x200 bytes per queue slot = 0x40 entries per slot)
     * Calculates descriptor index: `descriptor_index = (slot_index * 0x200) + queue_slot`
   * **Loop through pages**:
     * Sets `param_4[+0x26] = (uVar7 + 0xfff) >> 12` (number of 4KB pages needed)
     * For each page:
       * Writes physical address to descriptor array: `descriptor[descriptor_index] = aligned_address`
       * Calculates page-aligned chunk: `uVar2 = min(0x1000 - (address & 0xfff), remaining_length)`
       * Updates remaining length and SGL entry length
       * If SGL entry exhausted, moves to next SGL entry: `param_3[uVar5]` (linked list traversal)
       * Advances to next page-aligned address

4. **Final Setup**:
   * Sets `param_4[+0x50] = lVar3` (scatter-gather descriptor array pointer, or 0 if no SGL)
   * Returns `true` if `uVar7 == 0` (all data accounted for), `false` otherwise

**Key Structures**:
* **SGL Entry**: `param_3` points to linked list of entries
  * `param_3[0]` = physical address (qword)
  * `param_3[0][+0x10]` = length (dword)
  * `param_3[1]` = next SGL entry pointer (NULL-terminated)
* **Scatter-Gather Descriptor Array**: 
  * Located at `queue_handle[+0x8 + queue_index*8][+0x38]` or `[+0x40]`
  * Each descriptor is 8 bytes (physical address qword)
  * 0x200 bytes per queue slot = 64 descriptors per slot
  * Index calculation: `(slot_index * 0x200) + descriptor_index`

**Return Value**:
* `true` (1): Scatter-gather list successfully built, or no data transfer needed
* `false` (0): Scatter-gather list build failed (typically invalid SGL or insufficient descriptors)

**Called by**:
* `FUN_14000e2c8` - Queue type selection command (before submitting)
* `FUN_14000ea34` - Queue rotation command (before submitting)
* `FUN_14000ff50` - Special command handler (before submitting)

**Error Handling**:
* If `FUN_14000c9e4` returns `false`, the caller calls `FUN_14000c900` with error status
* This prevents submission of commands with invalid scatter-gather lists

**NOTE**: This function is critical for data transfer operations. It ensures all data pages are properly mapped and accessible for DMA operations before the command is submitted to hardware.

### `FUN_14000c1e4` - Command Configuration After State Machine Completion

**Location**: Analyzed from `rcbottom.sys`, but used by `rcraid.sys` functions
**Note**: Should verify if this function exists in `rcraid.sys` at the same address or if it's exported/imported between modules

**Purpose**: Configures command control register and flags after complex state machine operations complete. Called when state machine flags indicate all phases are done (`param_1[+0x73] & 0x37 == 0x37`).

**Parameters**:
* `param_1` (longlong): Queue handle pointer

**Logic**:
1. **Initial Setup**:
   * Sets `param_1[+0x3a0] = 1` (command ready flag)
   * Sets base control value: `param_1[+0x3a4] = 0x80110800`
   * Sets `param_1[+0x3b8] = queue[+10] - 1` (queue depth minus 1)

2. **Base Control Value Selection**:
   * If `param_1[+0xd1] & 3 == 1`: Changes base to `0x80510800` (different mode)

3. **Flag Setting Based on `devExt[+0x1c7a8]`** (from `param_1[+0x658]`):
   * **Source**: Extracted from descriptor blob during `FUN_140007d40` initialization (opcode 0x10, bits 4-9)
   * `== 8`: ORs `0x80` to control register
   * `== 4`: ORs `0x40` to control register
   * `== 2`: ORs `0x20` to control register

4. **Flag Setting Based on `devExt[+0x1c7ac]`** (from `param_1[+0x658]`):
   * **Source**: Extracted from descriptor blob during `FUN_140007d40` initialization (opcode 0x10, bits 0-3)
   * `== 5`: ORs `0x10` to control register
   * `== 4`: ORs `8` to control register
   * `== 2`: ORs `0x10000000` to control register

5. **Command Type Handling** (based on `param_1[+0xd4]`):
   * **`== 0x0c`**: 
     * Shifts `param_1[+0x3b0]` left by 3 bits
     * ORs `0x8000` to control register
   * **`== 0x09`**: 
     * ORs `0x200000` to control register
   * **Other**: 
     * Sets `param_1[+0x3a0] = 0` (command not ready)

6. **Final Command Submission** (if conditions met):
   * If `param_1[+0x6a4] != 0` AND `devExt[+0x1c7b0] == 0`:
     * Calls `FUN_14000e68c(devExt, param_1[+0x16178])` to submit the command

**Key Fields**:
* `param_1[+0x3a0]`: Command ready flag (1 = ready, 0 = not ready)
* `param_1[+0x3a4]`: Control register value (built from base flags + device-specific flags)
* `param_1[+0x3b0]`: Command parameter (shifted left by 3 for type 0x0c)
* `param_1[+0x3b8]`: Queue depth (used for queue management)
* `param_1[+0xd1]`: Mode flags (bits 0-1 control base value selection)
* `param_1[+0xd4]`: Command type (0x0c, 0x09, or other)
* `param_1[+0x658]`: Pointer to devExt (for accessing device-specific flags)
* `param_1[+0x6a4]`: Submission trigger flag
* `devExt[+0x1c7a8]`: Device-specific flag 1 (8, 4, or 2)
* `devExt[+0x1c7ac]`: Device-specific flag 2 (5, 4, or 2)
* `devExt[+0x1c7b0]`: Device state flag (0 = can submit)

**Called by**:
* `FUN_14000dae4` - After complex state machine completes (when `param_1[+0x73] & 0x37 == 0x37`)
* `FUN_14001023c` - Helper wrapper (checks `param_1[+0x15d01] == 0` first)

**Related Functions**:
* `FUN_14000e68c` - Command submission function called at the end if conditions are met
  * **Parameters**: `param_1` (devExt), `param_2` (dword parameter)
  * **Logic**:
    * Uses StorPort service `+0xbd8` to set up command submission context
    * Uses StorPort service `+0x650` to get device extension pointer
    * Stores `param_1` (devExt) and `param_2` (parameter) in context
    * Uses StorPort service `+0xbe0` to submit/activate command
    * Sets callback `FUN_14000e784` for completion handling
  * **Called by**: `FUN_14000c1e4` when `param_1[+0x6a4] != 0` and `devExt[+0x1c7b0] == 0`

* `FUN_14001023c` - Helper wrapper for `FUN_14000c1e4`
  * **Parameters**: `param_1` (devExt)
  * **Logic**: 
    * Checks if `param_1[+0x15d01] == 0` (condition check)
    * If true, calls `FUN_14000c1e4()` (no parameters - uses global state)
  * **Used by**: Callback installed in `FUN_140007d40` (firmware capability parsing)

**NOTE**: This function is the final step in the complex state machine path (`FUN_14000dae4`). It consolidates all state machine results into command control register values and triggers actual command submission if conditions are met. The control register value (`param_1[+0x3a4]`) contains all the flags needed for the command.

## Completion and Error Handling Functions (rcraid.sys)

### Completion Callback Functions

* **`FUN_14000e1ec`** - NVMe command completion callback with scatter-gather
  * **Parameters**: `param_1` (queue handle), `param_2` (completion queue descriptor), `param_3` (command descriptor), `param_4` (completion descriptor)
  * **Logic**:
    * Extracts SRB from `param_3[+0x18]` and completion callback from `param_3[+0x70]`
    * Checks completion status: If `param_4[+0xc] >= 0x20000`, sets error status (`param_3=2`, `param_4=0xc00` or `0x500`)
    * **Scatter-gather list cleanup**: If `SRB[+0x48] != NULL`:
      * Copies data from scatter-gather list to completion buffer using `FUN_140011140` (memcpy)
      * Builds linked list of scatter-gather entries starting at `param_2[+0x68]`
      * Updates `param_2[+0x70]` to point to last entry
    * Calls `FUN_14000c900` with SRB, callback, status, and error code
  * **Used by**: Commands with scatter-gather lists (cmd_type=9, callback `FUN_14000e1ec`)

* **`FUN_14001005c`** - Simple completion callback with size check
  * **Parameters**: `param_1` (unused), `param_2` (unused), `param_3` (command descriptor), `param_4` (completion descriptor)
  * **Logic**:
    * Extracts SRB from `param_3[+0x18]` and completion callback from `param_3[+0x70]`
    * Checks completion status: If `param_4[+0xc] >= 0x20000`, sets error status (`param_3=2`, `param_4=0xc00`)
    * Calls `FUN_14000c900` with SRB, callback, status, and error code
  * **Used by**: Simple commands (cmd_type=0x11, callback `FUN_14001005c`)

* **`FUN_140010184`** - Completion callback with SRB status update
  * **Parameters**: `param_1` (unused), `param_2` (unused), `param_3` (command descriptor), `param_4` (completion descriptor pointer)
  * **Logic**:
    * Extracts SRB from `param_3[+0x18]` and completion callback from `param_3[+0x70]`
    * **Updates SRB status fields**:
      * `SRB[+0x84] = param_4[0]` (status DWORD 0)
      * `SRB[+0x88] = param_4[1]` (status DWORD 1)
      * `SRB[+0x8c] = param_4[2] << 16 | param_4[10]` (status DWORD 2, packed)
      * `SRB[+0x90] = complex bit manipulation of param_4[3]` (status DWORD 3)
    * Checks completion status: If `param_4[3] >= 0x20000`, sets error status (`param_3=2`, `param_4=0xc00`)
    * Calls `FUN_14000c900` with SRB, callback, status, and error code
  * **Used by**: Commands that need status updates (cmd_type varies, callback `FUN_140010184`)

* **`FUN_1400097ac`** - Indirect completion handler
  * **Parameters**: `param_1` (SRB pointer)
  * **Logic**: Indirect call to function pointer at `DAT_140014c28` (StorPort completion handler)
  * **Used by**: `FUN_14000c900` when `param_2 == 0` (direct completion path)

### Queue Callback Handler

* **`FUN_14000eef8`** - Queue callback handler for completion processing
  * **Parameters**: `param_1` (queue handle), `param_2` (unused), `param_3` (completion descriptor pointer), `param_4` (completion queue entry)
  * **Logic**:
    * Checks completion type `param_3[0]`:
      * **Type 2**: If `param_3[+6] != NULL`, calls `FUN_14000c900(param_3[+6], param_3[+0x1c], 0, 0)` (successful completion)
      * **Type 7**: If `param_4[+0xc] < 0x20000`, sets queue flag `param_1[+0x398] |= 8` and calls `FUN_14000cb4c()` (queue processing)
      * **Type 0xd**: If `param_3[+0xc] != 0xa`, calls `FUN_14000d350()` (queue management command completion)
  * **Used by**: Queue completion processing (callback `FUN_14000eef8`)

### Command Routing Functions

* **`FUN_14000fafc`** - Primary queue dispatcher (NVMe command routing)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (unused)
  * **Logic**:
    * Clears scatter-gather handles: `param_2[+0x58] = 0`, `param_2[+0x48] = 0`
    * Routes based on `param_2[+0x14c]` (SRB function code):
      * **0x01 or 0x04**: Checks queue fullness, if full calls `FUN_14000e960`, else calls `FUN_14000ed2c`
      * **0x06**: Calls `FUN_14001026c`
      * **0x0a or 0x0b**: Checks queue fullness, if full calls `FUN_14000e960`, else calls `FUN_14000ec64`
      * **0x0c**: Checks queue fullness, if full calls `FUN_14000e960`, else calls `FUN_14000e2c8`
      * **Other**: Calls `FUN_14000f178` (NVMe command submission)
      * **< 0x0a**: Calls `FUN_14000c900(param_2, param_4, 2, 0x2000)` (unsupported)
    * **Queue fullness check**: `(queue[+6] + 1) % queue[+10] == queue[+8]` (producer + 1 == consumer)
  * **Returns**: 0 if queue full, 1 if command submitted
  * **Used by**: Primary NVMe queue dispatcher (callback `devExt+0x16100`)

* **`FUN_14000ed2c`** - Secondary queue dispatcher (command type routing)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (unused)
  * **Logic**:
    * Routes based on `param_2[+0x60]` (command type):
      * **0x12**: Calls `FUN_14000f838` (special command handler)
      * **0x05**: Calls `FUN_14000d190` (simple command)
      * **0x88 or 0x8a**: If `devExt[+0x15cd8] & 0x10`, calls `FUN_14000ea34` (queue rotation), else calls `FUN_14000c900` with error `0xc00` or `0x500`
      * **0x9e**: Calls `FUN_14000fa2c` (special command handler)
      * **0x1b**: Calls `FUN_14000c900(param_2, param_4, 0, 0)` (success)
      * **0x00**: Calls `FUN_14000c900(param_2, param_4, 0, 0)` (success)
      * **Other**: Calls `FUN_14000c900(param_2, param_4, 2, 0x2600)` (error)
  * **Returns**: 1 if command handled, 0 otherwise
  * **Used by**: Secondary queue dispatcher (called from `FUN_14000fafc`)

* **`FUN_14000ec64`** - Command router (SRB type routing)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (unused)
  * **Logic**:
    * Routes based on `param_2[+0x14c]` (SRB function) and `param_2[+0x60]` (command type):
      * **SRB 0x0b, command 0x02**: Calls `FUN_14000d3f4` (scatter-gather command)
      * **SRB 0x0b, command 0x09**: If `devExt[+0x15cd8] & 1`, calls `FUN_14000f06c`, else calls `FUN_14000c900` with error
      * **SRB != 0x0b, command 0x04**: Calls `FUN_14000fe44` (context command)
      * **SRB != 0x0b, command 0x09**: Calls `FUN_14000cf38` (SRB-based command)
      * **Other**: Calls `FUN_14000c900(param_2, param_4, 2, 0x2600)` (error)
  * **Returns**: 1 if command handled
  * **Used by**: Command router (called from `FUN_14000fafc`)

* **`FUN_14001026c`** - Command router for SRB function 0x06
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion callback)
  * **Logic**:
    * Routes based on `param_2[+0x60]` (command type):
      * **0x01**: Calls `FUN_14000ff50` (special command handler, cmd_type=0x11)
      * **0x02**: Calls `FUN_14000c900(param_2, param_4, 2, 0x2000)` (error: command not supported)
      * **Other**: Returns 0 (unhandled)
  * **Returns**: 1 if command handled (via `FUN_14000ff50` or `FUN_14000c900`), 0 if unhandled
  * **Used by**: Primary queue dispatcher (`FUN_14000fafc`) when SRB function code is 0x06
  * **Assembly details** (from Ghidra):
    * `MOVZX EDX,byte ptr [RDX + 0x60]` - reads command type
    * `SUB EDX,0x1` - subtracts 1 (optimization: 0x01 becomes 0, 0x02 becomes 1)
    * `JZ LAB_1400102ae` - if 0 (original was 0x01), jump to call `FUN_14000ff50`
    * `CMP EDX,0x1` - compares to 1 (checking if original was 0x02)
    * `JNZ LAB_1400102c8` - if not equal, jump to return 0
    * `CALL FUN_14000c900` - call error handler with status=2, error=0x2000
    * `CALL FUN_14000ff50` - call special command handler

### Special Command Handlers

* **`FUN_14000f838`** - Special command handler (type 0x12)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather pointer), `param_4` (unused)
  * **Logic**:
    * If `param_3 == NULL`, calls `FUN_14000c900(param_2, param_4, 2, 0x8407)` (invalid parameter)
    * Routes based on `param_2[+0x61]` (sub-command):
      * **Bit 0 clear**: Builds command buffer (0x88 bytes) with:
        * Header: `0x8000, 0x06, 0x02, 0x8001`
        * Controller ID from `devExt[+0x15c48]` (0x1bb1, 0x144d, or default)
        * Controller data from `devExt[+0x15c4c]` through `devExt[+0x15c88]`
        * Copies data from `param_3[+8]` (max 0x55 bytes) using `FUN_140011140`
      * **`param_2[+0x62] == 0x80`**: Builds 0x18-byte command buffer with `0x14008000` header
      * **`param_2[+0x62] == 0x86`**: Builds 0x40-byte command buffer with special header
      * **Other**: Calls `FUN_14000c900(param_2, param_4, 2, 0x2000)` (unsupported)
    * On success, calls `FUN_14000c900(param_2, param_4, 0, 0)`
  * **Returns**: 1 if handled
  * **Used by**: Special command routing (command type 0x12)

* **`FUN_14000fa2c`** - Special command handler (type 0x9e)
  * **Parameters**: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather pointer), `param_4` (unused)
  * **Logic**:
    * If `param_3 == NULL`, calls `FUN_14000c900(param_2, param_4, 2, 0x8407)` (invalid parameter)
    * If `param_2[+0x61] != 0x10` or `param_3[+0x10] < 0x20`, returns 1 (invalid)
    * **Copies controller serial number** from `devExt[+0x15cf0]` through `devExt[+0x15cf7]` to `param_3[+8]`:
      * 8 bytes copied in reverse order
      * Sets `param_3[+8+10] = 2` (format indicator)
    * Calls `FUN_14000c900(param_2, param_4, 0, 0)` (success)
  * **Returns**: 1 if handled
  * **Used by**: Special command routing (command type 0x9e)

## Vendor Mailbox Layout (rcbottom.sys)

The TRX50 firmware requires a vendor-specific "mailbox" structure embedded in the AHCI command table for vendor commands like `RC_CMD_SCAN_DISKS`. This mailbox is built dynamically by `FUN_140001008` with **no static template** - all fields are computed at runtime.

### Mailbox Construction (`FUN_140001008`)

* **Entry**: `(RCX=param_1, RDX=param_2, R8=param_3, R9=param_4, stack[0x28]=param_5, stack[0x30]=param_6)`
  * `param_1`: adapter context
  * `param_2`: **base of command table** (NOT the mailbox offset)
  * `param_3`: pointer to 8-byte command descriptor (opcode + flags)
  * `param_4`: pointer to 20-byte command payload (5 DWORDs)
  * `param_5`: boolean - enables extended mode (0x14-byte payload vs 0x8-byte)
  * `param_6`: boolean - controls flag bit 0x20000 at `+0x154`

* **Field writes** (all relative to `param_2`, the command table base):
  * `+0x10e`: 16-bit completion flags (values: `0x4400`, `0x1100`, `0x1400`, `0x4703`, or `0x0000`)
  * `+0x110`: Command type byte (always `0x02` when flags are set, else `0x00`)
  * `+0x111`: Control flags byte (copied from `param_3[2]` or `param_4[2]`)
  * `+0x112`: Length field (`0x08`, `0x14`, or `0x00`)
  * `+0x113`: Secondary control byte (copied from `param_3[3]` or `param_4[3]`)
  * `+0x114`–`+0x124`: Five DWORDs (copied from `param_4[0..16]` when `param_4[0] == 0x34`, else from `param_3[0..7]`)
  * `+0x154`: Extended flags DWORD (`|= 0x20000` when `param_5 == true && param_6 == false`)

### Conditional Logic (decoded from assembly)

1. **Command descriptor path** (`param_3 != NULL && param_3[0] == 0xA1`):
   * Sets `param_2[0x112] = 0x8` (8-byte payload)
   * Copies 8 bytes: `*(u64 *)(param_2 + 0x114) = *(u64 *)param_3`
   * Copies control bytes: `param_2[0x111] = param_3[2]`, `param_2[0x113] = param_3[3]`

2. **Command payload path** (`param_4 != NULL && param_4[0] == 0x34`):
   * Sets `param_2[0x112] = 0x14` (20-byte payload)
   * Copies 20 bytes: `memcpy(param_2 + 0x114, param_4, 20)`
   * Copies control bytes: `param_2[0x111] = param_4[2]`, `param_2[0x113] = param_4[3]`
   * **ACPI/GTF path**: If `param_4[2] & 0x1` and several other conditions pass, calls `FUN_140001df0(param_1, *(u32 *)(context + 0x8))` to inject ACPI data

3. **Fallback path** (neither condition met):
   * Zeroes the mailbox: `*(u16 *)(param_2 + 0x111) = 0`, `param_2[0x113] = 0`

4. **Extended flags** (`param_5 == true && param_6 == false`):
   * Sets bit 17: `*(u32 *)(param_2 + 0x154) |= 0x20000`

5. **Completion flags** (based on `param_2[0x111] & 0x20` and `param_2[0x113]` bits):
   * If `param_2[0x111] & 0x20`: writes `0x4400` to `param_2[0x10e]`
   * Else if `param_2[0x111] & 0x01`:
     * If `param_2[0x113] & 0x40`: writes `0x1100`
     * Else if `param_2[0x113] & 0x10`: writes `0x1400`
     * Else if `param_2[0x113] & 0x04` or `param_2[0x113] < 0`: writes `0x4703`
     * Else: writes `0x0000`
   * Else if `param_5 == false`: writes `0x0000`
   * Else: writes `0x0002` to `param_2[0x110]` only

### Struct Layout (confirmed from disassembly)

The mailbox spans `param_2 + 0x10e` through `param_2 + 0x154` (70 bytes):

```c
/* Offsets relative to AHCI command table base */
struct rc_vendor_mailbox {
    u16 completion_flags;  /* +0x10e: 0x4400/0x1100/0x1400/0x4703/0x0000 */
    u8  cmd_type;          /* +0x110: 0x02 or 0x00 */
    u8  control_flags;     /* +0x111: from param_3[2] or param_4[2] */
    u8  payload_length;    /* +0x112: 0x08, 0x14, or 0x00 */
    u8  secondary_control; /* +0x113: from param_3[3] or param_4[3] */
    u32 payload[5];        /* +0x114–0x124: command-specific data */
    u8  reserved[0x2c];    /* +0x128–0x153: not written by FUN_140001008 */
    u32 extended_flags;    /* +0x154: bit 17 = 0x20000 */
} __packed;
```

### Caller Context (`FUN_140001180`)

* Calls `FUN_140001008(param_1, lVar2, lVar1 + 0x58, lVar1 + 0x40, 1, 0)`
  * `lVar2`: command table base
  * `lVar1 + 0x58`: command descriptor pointer (8 bytes)
  * `lVar1 + 0x40`: command payload pointer (20 bytes)
  * `1`: param_5 = true (enable extended mode)
  * `0`: param_6 = false (set bit 0x20000)

### Linux Port Action Items

1. **Define struct**: Add `struct rc_vendor_mailbox` to `rc_linux.h` at the correct offset in the command table.
2. **Implement builder**: Add `rc_ahci_build_mailbox()` in `rc_queue.c` that mirrors `FUN_140001008` logic:
   * Zero the mailbox region (`cmd_table + 0x10e` through `cmd_table + 0x157`)
   * Set `payload_length = 0x14` for scan commands
   * Set `control_flags` and `secondary_control` based on command type
   * Copy command-specific payload (5 DWORDs) to `payload[0..4]`
   * Compute `completion_flags` using the bit-testing logic above
   * Set `extended_flags |= 0x20000` for normal commands
3. **Wire into command path**: Call `rc_ahci_build_mailbox()` from `rc_ahci_prepare_slot()` after building the FIS.
4. **Test with real hardware**: The exact values for `control_flags` and payload content must be determined by observing Windows driver behavior or firmware documentation.

## Remaining Unknowns

* Translate each confirmed StorPort slot to a Linux helper (e.g. `+0x1B8` → BAR
  template programming, `+0x3F0` → queue DMA allocation). The table above captures
  the observed Windows behaviour; we still need to map those to concrete kernel
  APIs (`pci_resource`, DMA setup, completion queues, etc.).
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

**Location**: `rcbottom.sys`

* `rcbottom` seeds the miniport callback table by storing function pointers into the device-extension block around offset `+0x8C0`.
* `FUN_1400079a4` writes the pointer for `FUN_140008f34` (BAR bring-up) into `devExt+0x8C0`, alongside neighboring slots that hold teardown and WMI handlers.
* The Windows driver populates this table before any BAR discovery; Linux port must mirror the same struct layout so the dispatcher trampolines (`FUN_140001438`, `FUN_1400027A8`, etc.) can be swapped in later phases.

### `FUN_1400079a4` – Front-door registration loop (Ghidra Analysis)

**Location**: `rcbottom.sys`

**Purpose**: Registers miniport callbacks for each port

**Entry**: `(RCX=devExt, RDX=descriptorPtr, R8=serviceCtx)`; pulls the port count from `*(descriptor+6)` and defaults to 1 when zero.

**Stack Layout** (from Ghidra):
* `local_f8`: 0x68-byte registration block (offset -0xf8)
* `local_e8`: Type/flag field (offset -0xe8)
* `local_e0`, `local_d8`, `local_d0`, `local_c8`: Callback function pointers
* `local_118`: Registration struct pointer at `devExt + (devExt+0xB0 + 6) * 8`
* `local_b8`: Descriptor pointer (param_2)
* `local_b0`: Service context (param_3)
* `local_88`: 0x40-byte secondary block (offset -0x88)
* `local_58`: Status byte returned from service slot +0x468

**Key Operations** (from Ghidra decompilation):
```c
// Get port count from descriptor
uVar2 = (uint)*(ushort *)(param_2 + 6);
if (*(ushort *)(param_2 + 6) == 0) {
    uVar2 = 1;  // Default to 1 port
}

uVar4 = (ulonglong)uVar2;
do {
    // Zero 0x68-byte registration block
    FUN_140011400(local_f8, 0, 0x68);
    
    // Set callback pointers
    local_e0 = FUN_14000751c;
    local_f8[0] = 0x68;        // Size field
    local_e8 = 2;              // Type/flag field
    local_d8 = FUN_140007428;
    local_9c = 2;
    
    // Conditional: if DAT_140014280 != 0, null out local_d8 and set bit in devExt+0x160fc
    if (DAT_140014280 != '\0') {
        local_d8 = (code *)0x0;
        *(uint *)(param_1 + 0x160fc) = *(uint *)(param_1 + 0x160fc) | 1;
        local_c0 = FUN_140007428;
    }
    
    local_d0 = FUN_1400074d4;
    local_c8 = FUN_1400073e0;
    local_e3 = 0;
    
    // Build registration struct at devExt + (devExt+0xB0 + 6) * 8
    local_118 = param_1 + ((ulonglong)*(uint *)(param_1 + 0xb0) + 6) * 8;
    local_b8 = param_2;  // Descriptor pointer
    local_b0 = param_3;  // Service context
    
    // Register via service slot +0x468
    iVar3 = (**(code **)(DAT_140014958 + 0x468))(
        DAT_140014980,
        *(undefined8 *)(param_1 + 0x20),  // devExt+0x20 (context)
        local_f8,
        0
    );
    
    if (iVar3 == 0) {
        // Success: increment port count
        *(int *)(param_1 + 0xb0) = *(int *)(param_1 + 0xb0) + 1;
        iVar1 = *(int *)(param_1 + 0xb0);
        
        // Store additional 0x40-byte block via service slot +0x4A8
        FUN_140011400(local_88, 0, 0x40);
        local_88[0] = 0x40;
        (**(code **)(DAT_140014958 + 0x4a8))(
            DAT_140014980,
            *(undefined8 *)(param_1 + 0x30 + (ulonglong)(iVar1 - 1) * 8),
            local_88
        );
        
        // Update devExt+0xB4 based on returned status
        *(bool *)(param_1 + 0xb4) = local_58 != (char)iVar3;
    }
    
    uVar4 = uVar4 - 1;
} while (uVar4 != 0);
```

**Structures**:
* Registration block: 0x68 bytes, size field at offset 0, type field at offset 0x28
* Secondary block: 0x40 bytes stored via service slot +0x4A8
* Callbacks stored at `devExt + (devExt+0xB0 + 6) * 8`

**Takeaway**: Linux adapter struct must expose the fields at `+0x20`, `+0x30`, `+0xB0`, `+0xB4`, `+0x160fc`. We also need equivalents for the Windows callback trio (`FUN_14000751c` etc.) when translating these registrations to blk-mq or worker initialization.

### `FUN_140008638` – Front door / PCI ID detection (Ghidra Analysis)

**Location**: `rcbottom.sys`

**Purpose**: "Front door" routine that handles PCI ID detection and initial adapter setup

**Device ID Detection** (from Ghidra):
```c
// Get device identification strings via service slot +0x3F0
// First call: reads device ID string 1 (0x400 bytes)
(**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980, param_2, 1, 0x400);
// Second call: reads device ID string 2 (0x400 bytes)
(**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980, param_2, 2, 0x400);

// Check for specific device IDs via wcsncmp
iVar3 = wcsncmp(local_838, L"PCI\\VEN_1022&DEV_7916", 0x15);  // Summit
if (iVar3 == 0) goto check_feature_flags;
iVar3 = wcsncmp(local_838, L"PCI\\VEN_1022&DEV_7917", 0x15);  // X570S
if (iVar3 == 0) goto check_feature_flags;
iVar3 = wcsncmp(local_838, L"PCI\\VEN_1022&DEV_7905", 0x15);  // Bristol
if (iVar3 == 0) goto check_feature_flags;

// Check for Promontory (43BD)
iVar3 = wcsncmp(local_838, L"PCI\\VEN_1022&DEV_43BD", 0x15);
if (iVar3 == 0) {
    bVar1 = (byte)DAT_1400146b0 & 1;  // Check bit 0 for 43BD
    goto check_feature_flags;
}

// Feature flag check: if feature bit not set, skip initialization
check_feature_flags:
if (bVar1 == 0) goto LAB_140008a19;  // Skip if feature not enabled
```

**Callback Registration** (from Ghidra):
```c
// Zero 0x90-byte callback registration block
FUN_140011400(local_8e8, 0, 0x90);

// Set up callback function pointers
local_8c0 = FUN_140008f34;   // BAR discovery
local_8b8 = FUN_14000911c;   // Unmap BAR
local_8e0 = FUN_140008a48;   // Queue init
local_8d0 = FUN_140008bc0;   // Descriptor/WMI registration
local_8d8 = FUN_140008b44;   // ?
local_8c8 = FUN_140008d88;   // Adapter object creation
local_880 = FUN_140009210;   // Teardown
local_878 = FUN_140009210;   // Teardown
local_888 = FUN_14000924c;   // Doorbell activation
local_870 = &LAB_14000918c;  // Safe dispatcher

local_8e8[0] = 0x90;  // Size field

// Register callbacks via service slot +0x1B8
(**(code **)(DAT_140014958 + 0x1b8))(DAT_140014980, param_2, local_8e8);
```

**Doorbell Activation Sequence** (CRITICAL FINDING):
After adapter setup in `FUN_140008638`, doorbells are rung in this order:
```c
// After FUN_140005ff4 returns adapter handle (lVar4):
(**(code **)(DAT_140014958 + 0x188))(DAT_140014980, lVar4, 1, 1);  // Doorbell 1
(**(code **)(DAT_140014958 + 0x188))(DAT_140014980, lVar4, 4, 1);  // Doorbell 4
(**(code **)(DAT_140014958 + 0x188))(DAT_140014980, lVar4, 2, 1);  // Doorbell 2
(**(code **)(DAT_140014958 + 0x188))(DAT_140014980, lVar4, 3, 1);  // Doorbell 3
```

**CRITICAL**: Doorbell order is **1, 4, 2, 3** (not 1, 2, 3, 4 as initially assumed!)

**Additional Setup**:
* Calls `FUN_140005ff4` to get adapter handle
* Sets up local configuration structure with values: `0x1c, 2, 2, 2, 2, 2, 1`
* Calls service slot `+0xE8` with configuration
* Calls `FUN_140007ba0` to parse devices
* Calls `FUN_14000a3d0` for handshake, then `FUN_140008540` if successful

**HMB Work Item Registration**:
* If descriptor table entry has feature bit `+0x2C & 0x8` and revision `+0x29 >= 4`, registers HMB work item via `FUN_140008368`

## BAR Discovery (FUN_140008f34 – updates)

* Entry arguments: `RCX=devExt`, `RDX=serviceTable`, `R8=HAL context`.
* First steps cache `R8` into `[RSP+0x18]` and call the StorPort service table to enumerate adapter resources.
* `MmMapIoSpace` is invoked after locating the memory BAR entry; result stored at `devExt+0x10` (base) with length in `devExt+0x18`. Flag byte at `devExt+0xB5` tracks BAR type.
* After mapping, the routine fans out into:
  * `FUN_140008a48` – queue spinlock bootstrap (writes callbacks at `+0x16110` and clears `+0x16054`).
  * `FUN_140008bc0` – descriptor/WMI table registration, gated by feature bits.
  * `FUN_140008d88` – adapter object creation and StorPort registrations.
  * `FUN_14000924c` – doorbell activation once queues are ready.
* Each StorPort service slot (`+0x1B8`, `+0x3F0`, `+0x650`, `+0x680`, `+0x980`, `+0x988`, etc.) must be mapped to Linux equivalents (pci_resource, dma_alloc, queue discovery).

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

**Location**: `rcbottom.sys`

### `FUN_14000924c` – Doorbell activation (Ghidra Analysis)

**Purpose**: Activates queues and rings firmware doorbells

**Entry**: `(RCX)` - single parameter (device extension or adapter context)

**Stack Layout** (from Ghidra):
* `local_res8`: Parameter 1 (RCX) cached at offset +0x8

**Key Operations** (from Ghidra and notes):
* Caches parameter at `[RSP+0x8]` (offset `14000924c`)
* Calls service slot `+0x650` to get adapter context
* Sets `devExt+0x16054 = 1` (adapter active flag)
* Calls `KeStallExecutionProcessor(5000)` at offset `140009285` (5 µs stall)
* If `devExt+0x16068 == 1` (legacy mode):
  * Calls `FUN_140001ed8` at offset `140009297` (legacy bring-up helper)
  * Calls `KeStallExecutionProcessor(25000)` at offset `1400092a1` (25 µs stall)
* Marks `devExt+0x1C2DC = 1` (firmware capability active)
* If `devExt+0x1607C` is still clear:
  * Calls `FUN_14000a564` at offset `1400092c0` (WMI/descriptor binder)
* Rings doorbells via service slot `+0x188` with indices 1, 4, 2, 3 (order from `FUN_140008638`)

**External Dependencies**:
* `KeStallExecutionProcessor` from HAL.DLL - called at offsets `140009285` (5µs) and `1400092a1` (25µs)

**Call Sites**:
* Called from `FUN_140008638` at offsets `140008871` (function pointer assignment) and `140008878` (stored in callback table)
* Also referenced at `1405d84c8` (likely driver initialization)

### `FUN_140001ed8` – Legacy queue bring-up helper (Ghidra Analysis)

**Purpose**: Legacy compatibility path for queue activation

**Stack Layout** (from Ghidra):
* `local_res20`: Saved register at offset +0x20
* `local_res18`: Saved register at offset +0x18
* `local_res10`: Saved register at offset +0x10
* `local_res8`: Saved register at offset +0x8
* `local_30`, `local_38`: Local variables

**Call Sites**:
* Called from `FUN_14000924c` at offset `140009297` when `devExt+0x16068 == 1` (legacy mode)
* Also referenced at `1405d8090` (likely another call site)

**Functionality**:
* Legacy helper that installs `FUN_14000924c` into the `devExt+0x16100` queue dispatcher when `DAT_1400146B0` bit0 is set (legacy compatibility mode).
* Walks 0x30 (48) controller slots, stepping `devExt` substructures in 0x728-byte strides.
* For each slot with a non-null context at `slot+0x6e0`, it:
  * Optionally calls the StorPort thunk at service offset `+0x9e0` when the global latch `DAT_140014731` is clear (acquire lock).
  * Clears the per-port queue tables stored at `[slot + index*8 + 0x8]` and `[slot + index*8 + 0x108]`.
  * Calls `FUN_140001008` to flush outstanding work and release any heap (`[RAX+0x838]`).
  * Uses `FUN_1400097ac` between entries, then zeroes slot-level pointers at `+0x2A4`/`+0x2B0` before releasing the lock via service `+0x9E8`.

### `FUN_14000a564` – WMI/descriptor binder (Ghidra Analysis)

**Purpose**: Binds WMI descriptors and prepares firmware capability data

**Stack Layout** (from Ghidra):
* `local_res20`: Saved register at offset +0x20
* `local_res18`: Saved register at offset +0x18
* `local_res10`: Saved register at offset +0x10
* `local_res8`: Saved register at offset +0x8

**Call Sites**:
* Called from `FUN_14000924c` at offset `1400092c0` when `devExt+0x1607C` is still clear
* Also referenced at `1405d8564` (likely another call site)

**Functionality**:
* Runs before doorbell activation to prepare WMI/descriptor data
* Called conditionally when `devExt+0x1607C` is still clear (early initialization phase)

**NOTE**: From `FUN_140008638` Ghidra analysis, the actual doorbell order called is **1, 4, 2, 3** (not sequential 1-4). This may be significant for device 0xb000 initialization and has been implemented in the Linux driver.


### `FUN_140007d40` – Firmware capability parsing (Ghidra Analysis)

**Location**: `rcbottom.sys`

**Purpose**: Parses firmware capabilities and installs callback functions based on detected controller variant

**Entry**: `(RCX)` - device extension parameter

**Stack Layout** (from Ghidra):
* `local_res20`: Saved register at offset +0x20
* `local_res18`: Saved register at offset +0x18
* `local_res10`: Saved register at offset +0x10
* `local_res8`: Saved register at offset +0x8
* `local_48`, `local_50`, `local_58`, `local_60`, `local_68`, `local_70`, `local_78`, `local_80`: Capability parsing variables
* `local_88`, `local_98`, `local_a8`, `local_b8`, `local_c8`: Additional capability data
* `local_d0`, `local_d4`, `local_d8`, `local_dc`, `local_e0`, `local_e4`, `local_e8`: Unicode/capability nibbles
* `local_f8`, `local_100`, `local_108`: Capability parsing buffers

**Call Sites**:
* Called from `FUN_140005ff4` at offset `140006402`
* Also referenced at `1405d8408` (likely driver initialization)

**Callback Functions Installed** (all installed by `FUN_140007d40`):
* `devExt+0x16100` (Primary queue dispatcher):
  * `FUN_140004090` - Installed at offsets 140007f46, 140007f4d
  * `FUN_14000fafc` - Installed at offsets 1400080c3, 1400080ca
  * `FUN_1400102d8` - Steady-state dispatcher

* `devExt+0x16108` (Early init wrapper):
  * `FUN_140001438` - Installed at offsets 140007f54, 140007f5b
  * `LAB_140001778` - Referenced at offsets 140007fe0, 140007fe7

* `devExt+0x16110` (Spinlock callback):
  * `FUN_1400021d4` - Installed at offsets 140007f62, 140007f69

* `devExt+0x16120` (Port disable/quiesce):
  * `FUN_140003048` - Installed at offsets 140007f8c, 140007f93

* `devExt+0x16128` (Mode toggle):
  * `FUN_1400027a8` - Installed at offsets 140007f9a, 140007fa1
  * `LAB_140002808` - Referenced at 8+ offsets

* `devExt+0x16130` (Port enable/resume):
  * `FUN_1400028f8` - Installed at offsets 140007fa8, 140007faf
  * `FUN_14000e494` - Temporary stub during early init (installed at offsets 14000813e, 140008145)

* `devExt+0x16140` (Status polling):
  * `FUN_140003f7c` - Installed at offsets 140007f70, 140007f77

* `devExt+0x16148` (Secondary queue helper):
  * `FUN_140003838` - Installed at offsets 140007fd2, 140007fd9
  * `FUN_14000d06c` - Installed at offsets 140008168, 14000816f

* Additional callbacks:
  * `FUN_14000303c` - Installed at 12+ offsets - frequently used helper
  * `FUN_140001ba4` - Installed at offsets 140007fb6, 140007fbd
  * `FUN_140001bbc` - Installed at offsets 140007fc4, 140007fcb
  * `FUN_140003598` - Installed at offsets 140007fee, 140007ff5
  * `FUN_14000c0bc` - Installed at offsets 1400080d8, 1400080e4
  * `FUN_14000c814` - Installed at offsets 14000814c, 140008153
  * `FUN_14000c82c` - Installed at offsets 14000815a, 140008161
  * `FUN_14000dd44` - Installed at offsets 1400080f2, 1400080ff
  * `FUN_14000e59c` - Installed at offsets 140008114, 14000811b
  * `FUN_14000e800` - Installed at offsets 140008122, 140008129
  * `FUN_1400100c0` - Installed at offsets 140008192, 140008199

* Safe dispatcher:
  * `LAB_14000918c` - Referenced at offsets 140008130, 140008137, 1400082cb, 1400082d2


**PCI Device ID Parsing** (from `FUN_140007d40` - decompiled implementation):
* Extracts 4 capability nibbles from device ID string using `wcsncpy`:
  * Nibble 1: offset +8, length 4 → stored at `devExt+0x16056`
  * Nibble 2: offset +0x11, length 4 → stored at `devExt+0x16058`
  * Nibble 3: offset +0x21, length 4 → stored at `devExt+0x1605a`
  * Nibble 4: offset +0x1d, length 4 → stored at `devExt+0x1605c`
* Uses `RtlInitUnicodeString` to initialize Unicode strings for comparison
* Uses `RtlUnicodeStringToInteger` with base 0x10 to convert hex strings to integers
* Uses `wcsncmp` to compare full PCI device ID strings (0x15 characters = 21 bytes):
  * `PCI\VEN_1022&DEV_7905` (Bristol Ridge) → `sVar5=0x82`, `sVar3=0xc6`
  * `PCI\VEN_1022&DEV_7916` (Summit Ridge) → `sVar5=0x108`, `sVar3=0x10a`
  * `PCI\VEN_1022&DEV_7917` (X570S) → `sVar5=0x108`, `sVar3=0x10a`
  * `PCI\VEN_1022&DEV_43BD` (TRX50/0x43bd) → triggers special path
* Callback installation logic:
  * **If device ID matches** (iVar7 == 1):
    * Sets `devExt+0x16068 = 1`, `devExt+0x1606c = 1`
    * Installs fast-path callbacks (all function pointers)
    * If `sVar5 != 0 && sVar3 != 0`, uses descriptor accessor to read capability blob
      * **Initializes descriptor accessor** via StorPort service `+0x418`:
        * Call: `(**(code **)(DAT_140014958 + 0x418))(DAT_140014980, devExt+0x20, &DAT_140012258, devExt+0x1c298)`
        * Parameters:
          * `DAT_140014980`: StorPort context
          * `devExt+0x20`: Device extension pointer
          * `&DAT_140012258`: Pointer to descriptor blob (static data structure containing firmware capabilities)
          * `devExt+0x1c298`: Output context pointer (stored descriptor accessor context)
        * This call sets up the descriptor accessor function pointer at `devExt+0x1c2d0` and context at `devExt+0x1c2a0`
      * **Device-specific opcodes**:
        * Device 0x7905: reads opcode `sVar5 = 0x82`, then opcode `sVar3 = 0xc6`
        * Device 0x7916/0x7917: reads opcode `sVar5 = 0x108`, then opcode `sVar3 = 0x10a`
      * **Capability word construction**: 
        * Reads first capability value into `local_e4[0]` using opcode `sVar5`
        * Reads second capability value into `local_e8[0]` using opcode `sVar3`
        * Combines: `iVar6 = (uint)local_e8[0] * 0x10000 + (uint)local_e4[0]` (high word from second, low word from first)
        * Stores packed capability word at `devExt+0x1c2d8`
  * **If no device ID match but `param_3 == 0`** (iVar7 == 99):
    * Sets `devExt+0x16068 = 99`, `devExt+0x1606c = 0`
    * Installs NVMe callbacks (queue_state=2 path)
  * **If no device ID match and `param_3 != 0`** (iVar7 == 2):
    * Sets `devExt+0x16068 = 2`, `devExt+0x1606c = 0`
    * Installs NVMe callbacks with descriptor accessor initialization
    * **Descriptor accessor sequence** (when `iVar7 == 2`):
      1. **Initializes descriptor accessor** via StorPort service `+0x418`:
         * Call: `(**(code **)(DAT_140014958 + 0x418))(DAT_140014980, devExt+0x20, &DAT_140012258, devExt+0x1c298)`
         * Parameters:
           * `DAT_140014980`: StorPort context
           * `devExt+0x20`: Device extension pointer
           * `&DAT_140012258`: Pointer to descriptor blob (static data structure)
           * `devExt+0x1c298`: Output context pointer (stored descriptor accessor context)
         * This call sets up the descriptor accessor function pointer at `devExt+0x1c2d0` and context at `devExt+0x1c2a0`
      2. Reads initial opcode: `(**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0), 0, local_e8, 0x34)`
         * Stores result in `local_e8[0]`, extracts low byte as next opcode: `uVar1 = local_e8[0] & 0xff`
      3. **Loops reading descriptors** (up to 10 iterations):
         * Reads next descriptor: `(**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0), 0, local_e4, uVar1)`
         * Checks if opcode is `0x10`: `if ((char)local_e4[0] == '\x10')`
         * If found, reads queue configuration: `(**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0), 0, local_d0, (byte)local_e8[0] + 0x12)`
         * Extracts queue configuration from `local_d0[0]`:
           * `devExt[+0x1c7ac] = local_d0[0] & 0xf` (bits 0-3: queue configuration field 1)
           * `devExt[+0x1c7a8] = (local_d0[0] >> 4) & 0x3f` (bits 4-9: queue configuration field 2)
         * Sets `uVar4 = 0` to exit loop
         * Otherwise, advances cursor: `uVar1 = local_e4[0] >> 8` (high byte becomes next opcode)
         * If loop exceeds 10 iterations, exits with `uVar4 = 0`
      4. **Queue configuration usage**: The extracted values at `devExt+0x1c7a8` and `devExt+0x1c7ac` are later used by `FUN_14000c1e4` to set control register flags:
         * `devExt[+0x1c7a8]` values: 8 → OR 0x80, 4 → OR 0x40, 2 → OR 0x20
         * `devExt[+0x1c7ac]` values: 5 → OR 0x10, 4 → OR 8, 2 → OR 0x10000000
  * **Otherwise** (fallback):
    * Installs safe dispatcher callbacks (all point to `FUN_1400102d8` or labels)

**Key Functionality**:
* Parses firmware ASCII/Unicode capability blob
* Stores four 16-bit values at `devExt+0x16056/58/5A/5C` (Unicode-configured nibbles)
* Sets queue variant flags (`devExt+0x16068`, `+0x1606C`)
* Installs callback table at `+0x16100`…`+0x16168` based on detected controller type
* If vendor strings match ("NVME", etc.) it selects the full fast-path callbacks
* Otherwise points everything at the safe dispatcher `FUN_1400102D8` / `LAB_14000918c`


### Callback Function Stack Layouts (from Ghidra)

All callback functions installed by `FUN_140007d40` have documented stack layouts:

**`FUN_140001438`** (Early init wrapper):
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_140001ba4`**:
* Stack: `local_res8`

**`FUN_140001bbc`**:
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_1400021d4`** (Spinlock callback):
* Stack: `local_res18`, `local_res10`, `local_res8`, `local_40`, `local_48`, `local_50`, `local_58`

**`FUN_1400027a8`** (Mode toggle):
* Stack: `local_res18`, `local_res10`, `local_res8`

**`FUN_1400028f8`** (Port enable/resume):
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`, `local_30`, `local_34`, `local_38`

**`FUN_14000303c`** (Frequently used helper):
* Stack: `local_res8`
* Called from `FUN_140007d40` at 12+ different offsets

**`FUN_140003048`** (Port disable/quiesce):
* Stack: `local_res18`, `local_res10`, `local_res8`, `local_40`, `local_44`, `local_48`

**`FUN_140003598`**:
* Stack: `local_res18`, `local_res10`, `local_res8`

**`FUN_140003838`** (Secondary queue helper):
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`, `local_50`, `local_58`, `local_5c`, `local_60`, `local_64`, `local_68`

**`FUN_140003f7c`** (Status polling):
* Stack: `local_res18`, `local_res10`, `local_res8`

**`FUN_140004090`** (Primary queue dispatcher):
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_14000c0bc`**:
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_14000c814`**:
* Stack: `local_res8`

**`FUN_14000c82c`**:
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_14000d06c`**:
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_14000dd44`**:
* Stack: `local_res18`, `local_res10`, `local_res8`, `local_40`, `local_48`, `local_50`, `local_58`, `local_60`, `local_64`, `local_68`, `local_70`, `local_78`, `local_80`, `local_88`

**`FUN_14000e494`** (Early init stub):
* Stack: `local_res8`

**`FUN_14000e59c`**:
* Stack: `local_res18`, `local_res10`, `local_res8`, `local_18`, `local_38`, `local_48`

**`FUN_14000e800`**:
* Stack: `local_res10`, `local_res8`

**`FUN_14000fafc`**:
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`

**`FUN_1400100c0`**:
* Stack: `local_res18`, `local_res10`, `local_res8`


### Callback Function Implementation Details (from Decompiled Code)

The following details are extracted from decompiled C code showing the actual implementation of each callback:

**`FUN_140001438`** (Early init wrapper - `devExt+0x16108`):
* Parameters: `param_1` (devExt), `param_2` (port context), `param_3` (SRB), `param_4` (completion callback)
* Logic:
  * Checks port state at `param_2+8` (byte offset)
  * Verifies queue bitmask at `devExt+0x3f8 + port_index*0x728` (bit 0x400 must be set)
  * Checks SRB state at `param_3+0x60` (must be 0 or 3)
  * Increments counter at `devExt+0x420 + port_index*0x728`
  * Calls `FUN_140001318` to handle port operations
  * If `param_4 == 0`, calls `FUN_1400097ac(param_3)` for SRB completion
  * Otherwise calls StorPort service `+0x838` for completion
* Returns: 1

**`FUN_140001ba4`** (Simple getter - `devExt+0x16138`):
* Parameters: `param_1` (devExt)
* Returns: Byte at `devExt+0x15920` (queue state flag)

**`FUN_140001bbc`** (Queue state check - `devExt+0x16140`):
* Parameters: `param_1` (devExt)
* Logic:
  * Checks `devExt+0xb4` (BAR type flag)
  * If BAR type is 0, reads queue mask from `devExt+0x158b8+8`
  * Stores mask at `devExt+0x158f4`
  * Checks bitmask intersection with `devExt+0x158e0`
  * If no active queues, sets register at `devExt+0x158b8+4` to `0x80000000`
* Returns: 1 if queues active, 0 if no queues

**`FUN_1400021d4`** (Spinlock callback - `devExt+0x16110`):
* Parameters: `param_1` (devExt), `param_2` (spinlock flag, 0=acquire, 1=release)
* Logic:
  * Saves previous state from `devExt+0x15920`
  * Sets `devExt+0x1c7b0` = `param_2`
  * Clears `devExt+0x15920` to 0
  * If MMIO base exists (`devExt+0x10`):
    * If `param_2 == 0` and previous state was 0, calls StorPort service `+0x100` for adapter handle
    * Calls `FUN_14000b730` for initialization
    * If `devExt+0x15930 == 0`, initializes 8 queues via `FUN_14000be7c` (offset 0x650, stride 0x728)
  * Polls register at `devExt+0x10+1` (offset 1) with `KeStallExecutionProcessor(1000)` delays
  * Waits up to 1000 iterations for bit 0 to clear
  * Sets register bit 31 (`0x80000000`) when ready
  * Reads queue configuration from register at offset 3
  * Calculates queue sizes: `queue_count = (reg[3] & 0x1f) + 1`, `queue_depth = ((reg[3]>>8) & 0x1f) + 1`
  * Allocates DMA memory for queues via StorPort services
  * **Calls service slot `+0x9D8` (completion register programming) three times:**
    * **First call** (offset `1400023b0`): `(serviceCtx, 0, devExt + 0x15928)` - programs completion register context
    * **Second call** (offset `140002648`, in queue loop): `(serviceCtx, 0, queue_desc + 0x6f0)` - programs per-queue completion registers
    * **Third call** (offset `140002665`, in queue loop): `(serviceCtx, 0, queue_desc + 0x6e8)` - programs per-queue completion registers
* **NOTE**: Service slot `+0x9D8` is also called from:
  * `FUN_140008a48`: `devExt + 0x16288` and `devExt + 0x1c2e8` (per-BAR contexts)
  * `FUN_140007978`: `&DAT_140216880` (global context)
  * Programs queue descriptors for all 8 ports
  * Calls `FUN_140002ef0` for completion structure setup
* **Key offsets used:**
  * `devExt + 0x157b8` - Read after first service slot call (likely completion register context size/config)
  * `devExt + 0x157f0` / `devExt + 0x15928` - Completion register context passed to service slot
  * `queue_desc + 0x6e8` / `queue_desc + 0x6f0` - Per-queue completion register contexts
* **NOTE**: Service slot `+0x9D8` is a runtime function pointer (StorPort service), so the actual completion register programming code is in the StorPort library, not in `rcbottom.sys`. To find the actual register writes, search for MMIO writes to offset `0x100+` in the StorPort library or look for direct register writes in the listing view.
* This is the critical queue initialization function

**`FUN_1400027a8`** (Mode toggle - `devExt+0x16128`):
* Parameters: `param_1` (devExt), `param_2` (unused), `param_3` (mode flag)
* Logic:
  * If `param_3 != 0` and `devExt+0xb4 == 0`:
    * Sets register at `devExt+0x158b8+4` to `0x80000002` (mode toggle bit)

**`FUN_1400028f8`** (Port enable/resume - `devExt+0x16130`):
* Parameters: `param_1` (devExt)
* Logic:
  * Very complex state machine processing port queues
  * Iterates through all active ports (up to 48, stride 0xe5)
  * State machine with states 0-15:
    * State 0: Port enable, sets bitmasks
    * State 1: Calls `FUN_140002ef0` for completion setup
    * State 2: Sets queue register to `0x70000000`, advances timestamp
    * State 3: Calls `FUN_1400014dc` for SRB allocation
    * State 4: Calls `FUN_14000484c` for port configuration
    * State 5: Calls `FUN_140004578` for queue programming
    * State 6: Calls `FUN_140004c10` for controller settings
    * State 7: Handles port enable with nested queue activation
    * State 8: Sets register bit 2, advances to state 9
    * State 9: Sets completion flags, calls `FUN_140002ef0`
    * State 10: Handles command completion, checks SRB status
    * State 12: Calls `FUN_140001868` for doorbell activation
  * Calls various helper functions: `FUN_1400058dc`, `FUN_1400014dc`, `FUN_1400016b0`, `FUN_1400036e0`, `FUN_14000b500`
* This is the main port resume/enable path

**`FUN_14000303c`** (No-op - `devExt+0x16118`, `+0x16158`):
* Empty function - returns immediately
* Used as placeholder for optional callbacks

**`FUN_140003048`** (Port disable/quiesce - `devExt+0x16120`):
* Parameters: `param_1` (devExt), `param_2` (port mask, 0xffffffff = all)
* Logic:
  * Iterates through ports (up to `devExt+0x158fc` count)
  * For each active port:
    * Reads port register at offset 0x10
    * If register == 0, calls `FUN_14000330c` to snapshot queue pointers
    * Otherwise processes pending requests:
      * Checks bit 0x1e (bit 30) for completion flag
      * Calls `FUN_14000438c` and `FUN_14000403c` to flush SRBs
      * Calls `FUN_140001318` for port disable
      * Calls `FUN_140001868` for doorbell cleanup
      * Calls `FUN_14000330c` to hand off to firmware
  * Handles error conditions and queue state transitions
* This is the port shutdown/disable path

**`FUN_140003598`** (Queue cleanup - `devExt+0x16160`):
* Parameters: `param_1` (devExt)
* Logic:
  * Iterates through all 48 ports (stride 0x1ca, offset 0x410)
  * For each active port:
    * Checks if port state byte is `\n` (0x0A)
    * If so, calls `FUN_1400016b0` for queue cleanup

**`FUN_140003838`** (Secondary queue helper - `devExt+0x16148`):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather list), `param_4` (completion)
* Logic:
  * Determines target port from SRB (`param_2+0x38+8`)
  * Selects queue based on port state and command type
  * Checks if queue slot is available (bitmask check)
  * If available:
    * Builds command descriptor in queue slot
    * Sets command flags based on SRB type (0x01, 0x04, 0x07, 0x08)
    * Copies scatter-gather entries to command table
    * Updates queue producer index
    * Calls `FUN_1400027dc` to check if doorbell should be rung
    * If doorbell needed, updates MMIO registers and rings doorbell
  * Returns: 1 on success, 0 on queue full

**`FUN_140003f7c`** (Status polling cleanup - `devExt+0x16118`):
* Parameters: `param_1` (devExt)
* Logic:
  * Releases spinlock via StorPort service `+0x680`
  * Clears `devExt+0x15928` and `+0x15908`
  * Iterates through all 8 ports (stride 0xe5):
    * Clears port state byte
    * Releases port spinlocks
    * Clears port queue pointers

**`FUN_140004090`** (Primary queue dispatcher - `devExt+0x16100`):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather), `param_4` (completion)
* Logic:
  * Clears `param_2+0x58` (SRB status)
  * Gets port index from `param_2+0x38+8`
  * Checks port state at `devExt+0x410 + port_index*0x728`
  * Routes based on SRB command type (`param_2+0x14c`):
    * Type 0x01, 0x04: Read/Write commands
    * Type 0x03: Calls `FUN_140003d94` for port control
    * Type 0x09: Calls `FUN_140003838` for secondary queue
  * Default: Calls `FUN_140003838` for command submission
* Returns: 1

**`FUN_140004170`** (Command routing helper):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather), `param_4` (completion)
* Logic:
  * Gets port index from `param_2+0x38+8`
  * Gets port descriptor from `devExt + 0x158 + port_index*0x728`
  * Routes based on command type (`param_2+0x14c`):
    * If command type is 0x07 or 0x08 (special commands):
      * Checks port state - if state < 10, calls `FUN_140003838` or `FUN_140003d94`
    * Otherwise:
      * Calls `FUN_140001008` to build vendor mailbox
      * Completes SRB with error or success
* Returns: 1 on success, 0 on error

**`FUN_140003d94`** (AHCI command submission):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (completion callback)
* Logic:
  * Uses StorPort service `+0x310` to get adapter handle
  * Uses StorPort service `+0x318` to submit command with callback `FUN_1400075ac`
  * Uses StorPort service `+0x328` to check completion status
  * If submission fails, releases handle and completes SRB with error
  * The callback `FUN_1400075ac` allocates scatter-gather list and calls callback at `devExt+0x16148`
* Returns: void

**`FUN_1400075ac`** (Command submission callback):
* Parameters: `param_1` (adapter handle), `param_2` (unused), `param_3` (SRB context), `param_4` (unused), `param_5` (scatter-gather list pointer)
* Logic:
  * Releases spinlock via StorPort service `+0x350`
  * Allocates pool for scatter-gather list (`ExAllocatePoolWithTag`, tag `0x72634148`)
  * Builds scatter-gather list from `param_5` array
  * Sets bit 0x100000 in SRB flags
  * Calls callback at `devExt+0x16148` (secondary queue helper) with scatter-gather list
  * If callback fails, frees pool and clears bit
* Returns: 1 on success, 0 on failure

**`FUN_14000f178`** (NVMe command submission):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (completion callback)
* Logic:
  * Similar to `FUN_140003d94` but for NVMe path
  * Uses StorPort services `+0x310`, `+0x318`, `+0x328` for command submission
  * Calls `FUN_14000f79c` or `FUN_14000f718` for command preparation
  * Uses callback `FUN_1400075ac` for scatter-gather handling
* Returns: void

**`FUN_14000f454`** (NVMe queue initialization):
* Parameters: `param_1` (queue descriptor pointer), `param_2` (adapter handle), `param_3` (queue index), `param_4` (sub-queue index), `param_5` (init flag), `param_6` (enable flag), `param_7` (completion queue base), `param_8` (submission queue base)
* Logic:
  * Initializes queue descriptor structure (0x78 bytes)
  * Sets queue ID, queue size (max 0x100 entries, clamped)
  * Programs completion queue base at `psVar1+0xc` (param_7)
  * Programs submission queue base at `psVar1+0x14` (param_8)
  * Sets up completion queue entries (0x100 entries, 0x78 bytes each)
  * If `param_6 == 0`, calls `FUN_14000f608` to enable queue
* This is the NVMe-specific queue setup, separate from AHCI completion registers

**`FUN_140010488`** (MMIO register I/O - likely not completion registers):
* Parameters: `param_1` (register context), `param_2` (unused), `param_3` (buffer descriptor)
* Logic:
  * Writes to MMIO offset `0x10` (not `0x100+` - so this is likely command/control registers)
  * Reads from offsets `0x34`, `0x78`, `0x80`, `0x9c`
  * Writes values `0x100`, `0x200`, or `0x300` based on buffer state
  * Uses `KeStallExecutionProcessor(0x19)` (25µs) delays for polling
  * This appears to be a generic register I/O function, not completion register specific
* Returns: 0 on success, 0xffffffff on failure

**`FUN_14000c0bc`** (NVMe command submission - `devExt+0x16108` for queue_state=2):
* Parameters: `param_1` (devExt), `param_2` (unused), `param_3` (SRB), `param_4` (completion)
* Logic:
  * Gets NVMe queue from `devExt+0x15940`
  * Allocates command slot via `FUN_14000c2fc`
  * If `param_3 == 0`, releases queue and returns
  * Otherwise:
    * Programs command DWORDs 0-3 (command type, flags, NSID)
    * Sets completion callback at `param_3+0xc4`
    * Copies command to submission queue (64-byte entries)
    * Updates queue producer index with modulo arithmetic
* Returns: 1 on success, 0 on queue full

**`FUN_14000c814`** (NVMe state getter):
* Parameters: `param_1` (devExt)
* Returns: Byte at `devExt+0x15d00` (NVMe queue state)

**`FUN_14000c82c`** (NVMe completion check):
* Parameters: `param_1` (devExt), `param_2` (port index, -1 = all)
* Logic:
  * If `param_2 == -1`:
    * Checks all NVMe queues for completion
    * Compares submission queue head vs tail
  * Otherwise:
    * Checks single queue
    * Sets completion register bitmask
* Returns: true if completions pending

**`FUN_14000d06c`** (NVMe secondary queue dispatcher - `devExt+0x16148` for queue_state=2):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (scatter-gather), `param_4` (completion callback)
* Logic:
  * Gets SRB function code from `param_2+0x14c`
  * **For SRB function 0x01 or 0x04** (Read/Write):
    * Gets queue handle from `devExt+0x15948 + (devExt+0x15968 * 8)`
    * **Queue fullness check**: `(queue[+6] + 1) % queue[+10] == queue[+8]`
      * `queue[+6]` = producer index (ushort)
      * `queue[+10]` = queue depth (ushort)
      * `queue[+8]` = consumer index (ushort)
    * If queue NOT full: Calls `FUN_14000ed2c` for command submission
    * If queue full: Calls `FUN_14000e960` for deferred processing
  * **For SRB function 0x0a**:
    * Gets queue handle from `devExt+0x15948 + (devExt+0x15968 * 8)`
    * **Queue fullness check**: Same as above
    * If queue NOT full: Calls `FUN_14000ec64` for command routing
    * If queue full: Calls `FUN_14000e960` for deferred processing
  * **For other SRB functions**:
    * Sets error status: `param_2[+0x100] = param_2[+0x13c]`, `param_2[+0x110] = 2`, `param_2[+0x112] = 0`
    * If `param_4 != 0`: Calls service `+0x838` with `param_4` to release completion handle
    * Else: Calls `FUN_1400097ac` for SRB completion
    * Returns 1
* Returns: 1 on success, 0 on queue full (deferred via `FUN_14000e960`)
* Used by: Secondary queue dispatcher (callback `devExt+0x16148`)

**`FUN_14000dd44`** (NVMe initialization - `devExt+0x16110` for queue_state=2):
* Parameters: `param_1` (devExt), `param_2` (init flag)
* Logic:
  * Extensive NVMe queue setup:
    * Allocates submission/completion queues (0x30000 bytes per queue)
    * Sets up queue descriptors (0x78 bytes each)
    * Programs queue doorbell registers
    * Initializes queue state tracking
  * Reads controller capabilities from MMIO
  * Sets up 1-4 queues based on `devExt+0xb0` (port count)
  * Programs completion queue entries (0x78 bytes each, 0x100 entries)
  * Calls `FUN_14000f454` for each queue initialization
* This is the NVMe-specific initialization path

**`FUN_14000e494`** (Early init stub - `devExt+0x16130` temporary):
* Parameters: `param_1` (devExt)
* Logic:
  * Checks `devExt+0x16074` flag
  * If set, calls `FUN_14000fca4`
  * Checks `devExt+0x16075` flag
  * If set, calls `FUN_14000fd10`
* This is a temporary stub replaced by `FUN_1400028f8` after full init

**`FUN_14000e59c`** (NVMe completion processing):
* Parameters: `param_1` (devExt)
* Logic:
  * Acquires spinlock
  * Releases all NVMe queue resources
  * Calls `FUN_140005f90` for queue state check
  * Polls completion queues with `KeStallExecutionProcessor(1000)` delays
  * Waits up to 2000 iterations (2 seconds) for completions
  * Updates MMIO registers for queue state
  * Calls `FUN_14000fdd8` for final cleanup

**`FUN_14000e800`** (NVMe command submission wrapper):
* Parameters: `param_1` (devExt), `param_2` (port index)
* Logic:
  * Calls `FUN_14000e820(param_1, param_2, 1)` for command submission

**`FUN_14000fafc`** (NVMe primary queue dispatcher - `devExt+0x16100` for queue_state=2):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion callback)
* Logic:
  * Clears scatter-gather handles: `param_2[+0x58] = 0`, `param_2[+0x48] = 0`
  * Gets SRB function code from `param_2+0x14c`
  * Gets scatter-gather pointer from `param_2+0x178`
  * Routes based on SRB function code:
    * **0x01 or 0x04** (Read/Write):
      * Gets queue handle from `devExt+0x15948 + (devExt+0x15968 * 8)`
      * **Queue fullness check**: `(queue[+6] + 1) % queue[+10] == queue[+8]`
      * If queue full: Calls `FUN_14000e960` for deferred processing, returns 0
      * If queue NOT full and `param_2[+0x178] == 0`: Calls `FUN_14000ed2c` for command submission
      * Else: Calls `FUN_14000f178` (NVMe command submission with scatter-gather)
    * **0x06**: Calls `FUN_14001026c` (command router for SRB function 0x06)
    * **0x0a or 0x0b**:
      * Gets queue handle from `devExt+0x15948 + (devExt+0x15968 * 8)`
      * **Queue fullness check**: Same as above
      * If queue full: Calls `FUN_14000e960` for deferred processing, returns 0
      * If queue NOT full and `param_2[+0x178] == 0`: Calls `FUN_14000ec64` for command routing
      * Else: Calls `FUN_14000f178` (NVMe command submission)
    * **0x0c**:
      * Gets queue handle from `devExt+0x15948 + (devExt+0x15968 * 8)`
      * **Queue fullness check**: Same as above
      * If queue full: Calls `FUN_14000e960` for deferred processing, returns 0
      * If queue NOT full and `param_2[+0x178] == 0`: Calls `FUN_14000e2c8` for command submission
      * Else: Calls `FUN_14000f178` (NVMe command submission)
    * **< 0x0a**: Calls `FUN_14000c900(param_2, param_4, 2, 0x2000)` (unsupported command)
    * **Other**: Calls `FUN_14000f178` (NVMe command submission)
* Returns: 0 if queue full (deferred via `FUN_14000e960`), 1 if command submitted
* Used by: Primary NVMe queue dispatcher (callback `devExt+0x16100`)

**`FUN_14000e960`** (Queue full handler - deferred work item queue):
* Parameters: `param_1` (devExt), `param_2` (SRB), `param_3` (unused), `param_4` (completion callback)
* Purpose: Handles queue-full conditions by queuing work items for deferred processing
* Logic:
  * **Allocates work item structure** (0x28 bytes) via `ExAllocatePoolWithTag(0x200, 0x28, 0x72634148)`:
    * Tag: `0x72634148` = "HAcr" (ASCII reversed)
    * Pool type: `0x200` (NonPagedPool)
    * Size: 0x28 bytes (40 bytes)
  * **Work item structure layout**:
    * `puVar1[0]` = 0 (next pointer, initialized to 0)
    * `puVar1[1]` = 0 (previous pointer, initialized to 0)
    * `puVar1[2]` = `param_2` (SRB pointer)
    * `puVar1[3]` = `param_3` (unused parameter)
    * `puVar1[4]` = `param_4` (completion callback)
  * **Queue insertion** (protected by spinlocks):
    * Acquires spinlock via service `+0x9e0` with `devExt+0x15f80`
    * If queue is empty (`devExt+0x15f88 == 0`):
      * Sets `devExt+0x15f88 = puVar1` (head pointer)
    * Else:
      * Links new item: `*(devExt+0x15f90) = puVar1` (sets next pointer of tail)
    * Sets `devExt+0x15f90 = puVar1` (tail pointer)
    * Releases spinlock via service `+0x9e8` with `devExt+0x15f80`
  * **Work item queue structure**:
    * `devExt+0x15f80`: Spinlock handle for work item queue
    * `devExt+0x15f88`: Head pointer to first work item (linked list start)
    * `devExt+0x15f90`: Tail pointer to last work item (linked list end)
* Returns: void
* Used by: Called when queue is full in `FUN_14000fafc`, `FUN_14000d06c` (and other queue dispatchers)
* Note: Work items are processed later (likely by a worker thread or ISR) to retry command submission when queue space becomes available

**`FUN_1400100c0`** (Error handling/logging):
* Parameters: `param_1` (devExt), `param_2` (error flags pointer)
* Logic:
  * Checks `devExt+0x15fa0` (error logging enabled flag)
  * Processes error flags from `param_2`
  * Updates error state at `devExt+0x15fa4`, `+0x15fa8`
  * Decodes error codes and calls `FUN_1400107b8` for logging

**`FUN_14001023c`** (NVMe initialization check - `devExt+0x16160` for queue_state=2):
* Parameters: `param_1` (devExt)
* Logic:
  * Checks `devExt+0x15d01` flag
  * If clear, calls `FUN_14000c1e4` for NVMe initialization

**`FUN_1400102d8`** (Steady-state dispatcher - safe path):
* Parameters: None (varargs)
* Returns: 1
* This is the default safe dispatcher when firmware capability parsing fails

**`FUN_1400102f4`** (Dispatcher stub):
* Returns: 1
* Used as placeholder in safe dispatcher path

**`LAB_140010304`, `LAB_140010318`, `LAB_140010328`** (Parameter marshalling labels):
* These are code labels used for parameter marshalling in the safe dispatcher path
* `LAB_140010304`: Stores DWORD parameter
* `LAB_140010318`: Stores BYTE parameter
* `LAB_140010328`: Stores DWORD parameter


**`FUN_14001023c`**:
* Stack: `local_res8`
* Installed at offsets 140008184, 14000818b

**`FUN_1400102d8`** (Steady-state dispatcher):
* Stack: `local_res20`, `local_res18`, `local_res10`, `local_res8`
* Installed at 6 offsets: 140008277, 14000827e, 140008285, 14000828c, 140008303, 14000830a
* This is the main dispatcher installed when firmware capability parsing selects the safe path

**`FUN_1400102f4`**:
* Stack: `local_res8`
* Installed at offsets 1400082e7, 1400082ee
* Has labels: `LAB_140010304`, `LAB_140010318`, `LAB_140010328` (parameter marshalling)


### Descriptor accessor @`devExt+0x1C2D0`

* Installed during `FUN_140007d40` when the driver parses the PCI hardware ID strings. The same routine also seeds the callback arrays at `+0x16100`…`+0x16168` and caches vendor-specific fields at `+0x16056/58/5A/5C`.
* **Initialization**: Uses StorPort service `+0x418` to initialize the descriptor accessor:
  * Call signature: `(**(code **)(DAT_140014958 + 0x418))(StorPortContext, devExt+0x20, &DAT_140012258, devExt+0x1c298)`
  * **Parameters**:
    * `DAT_140014980`: StorPort context pointer
    * `devExt+0x20`: Device extension pointer (passed to service)
    * `&DAT_140012258`: Pointer to static descriptor blob (firmware capability data structure)
    * `devExt+0x1c298`: Output parameter - descriptor accessor context (stored here)
  * **Output**: After this call:
    * `devExt+0x1c2d0`: Contains function pointer to descriptor accessor function
    * `devExt+0x1c2a0`: Contains descriptor accessor context (pointer to internal descriptor table)
* **Usage**: The descriptor accessor function is called as `(**(code **)(devExt+0x1c2d0))(devExt+0x1c2a0, flags, outBuf, opcode, width)`
* **Descriptor blob `DAT_140012258`**: Static data structure containing firmware capability information. The blob content we captured from Ghidra (little-endian qwords) is:

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

> **Need from Ghidra:** In the Listing view, search for `mov [rbx+0x1c2d0],` to locate the store that seeds this function pointer, open that callee, and dump its disassembly. That routine (the "descriptor accessor") walks the `DAT_140012258` blob and implements opcodes `0x05`, `0x10`, `0x11`—we still need its exact control flow to finish decoding the descriptor format.
> **POTENTIAL FINDING**: `0x1c2d0` may be linked to function at `14002ce29` - verify if this is the descriptor accessor implementation or initialization point.

### `FUN_140006e3c` – Completion processing and adapter iteration

* **Purpose**: Processes queue completions from all adapters and builds completion data structures for WMI/StorPort.
* **Adapter iteration**: 
  * Gets adapter list context using `devExt+0x16020` via service `+0x650` with `PTR_DAT_140014040`
  * Iterates through all adapters in the list (up to adapter count at `lVar10+0x10`)
  * For each adapter, accesses `devExt+0x16020` via `*(longlong *)(lVar10 + 0x18 + uVar20 * 8) + 0x16020`
* **Queue state checking**:
  * For legacy adapters (`devExt+0x16068 == 1`): Checks `devExt+0x15908`, `devExt+0x15910`, `devExt+0x15918` for active queues
  * For NVMe adapters (`devExt+0x16068 == 2`): Checks `devExt+0x15ce0` and `devExt+0x15ce1` for completion state
* **Completion data building**:
  * Allocates completion buffer via service `+0x850`
  * Builds completion descriptors (0x68 bytes each) from queue entries
  * Processes up to 32 completions (`0x20` limit)
  * Handles three queue types: primary (`devExt+0x15908`), secondary (`devExt+0x15910`), tertiary (`devExt+0x15918`)
* **Completion descriptor structure** (0x68 bytes):
  * `+0x18`: Queue index/port number
  * `+0x24`: Queue type (0x1=primary, 0x2=secondary, 0x3=tertiary)
  * `+0x28`: Queue depth/size
  * `+0x2c`: Completion type
  * `+0x30`: Adapter index
  * `+0x34`: Status flags
  * `+0x38-0x60`: Completion data (DMA addresses, command data)
* **Special handling**:
  * For NVMe adapters: Extracts completion data from `devExt+0x15ce8`, `devExt+0x15ce4`, `devExt+0x15cf8`, `devExt+0x15d18`
  * For legacy adapters: Extracts data from queue descriptor structures at offsets `+0x8`, `+0x301`, `+0x2c0`, `+0x518-0x530`
* **Final step**: Calls service `+0x838` to finalize completion processing

### `FUN_14000a564` / `FUN_14000a72c` – Multi-adapter WMI/descriptor binding

* **`FUN_14000a564`**: Disconnects/removes adapter from multi-adapter group
  * **Purpose**: Removes adapter from multi-adapter WMI binding when `DAT_140014290 > 1` and `DAT_140014730 != 0`
  * **Adapter iteration**: 
    * Gets adapter list context using `devExt+0x16018` via service `+0x650` with `PTR_DAT_140014040`
    * Iterates through all adapters, accessing each adapter's `devExt+0x16020`
    * Gets adapter handle via service `+0x6e8` for each `devExt+0x16020`
    * Calls service `+0x108` twice per adapter (disconnect operations)
    * Calls service `+0xc18` to disconnect adapters from each other (except current adapter at `DAT_140014734`)
  * **Final step**: Sets `DAT_140014730 = 0` (disconnected state)

* **`FUN_14000a72c`**: Connects/adds adapter to multi-adapter group
  * **Purpose**: Adds adapter to multi-adapter WMI binding when `DAT_140014290 > 1` and `DAT_140014730 == 0`
  * **Parameters**: `param_1` (adapter handle), `param_2` (adapter index)
  * **Adapter iteration**:
    * Gets adapter list context using `param_1` via service `+0x650` with `PTR_DAT_140014040`
    * Gets current adapter handle via service `+0x6e8` using `param_1`
    * Sets `DAT_140014734 = param_2` (current adapter index)
    * Iterates through all adapters, accessing each adapter's `devExt+0x16020`
    * Gets adapter handle via service `+0x6e8` for each `devExt+0x16020`
    * Calls service `+0x108` twice per adapter (connect operations)
    * Calls service `+0x118` to connect adapters to each other (except current adapter)
    * Calls service `+0x120` to finalize connection
  * **Final step**: Sets `DAT_140014730 = 1` (connected state)

### Descriptor Accessor Usage in `FUN_140008f34` (Ghidra Analysis)

From the decompiled `FUN_140008f34`, the descriptor accessor is used when `devExt+0x16068 == 2`:

```c
if (*(int *)(lVar6 + 0x16068) == 2) {
    // Initial call: opcode 0x34, width 1
    (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0), 0, local_38, 0x34, 1);
    uVar3 = (ushort)local_38[0];
    
    local_30 = 0;
    local_2c = 0;
    uVar5 = 1;
    uVar10 = 0;
    
    // Loop: read capability descriptors
    do {
        uVar10 = uVar10 + 1;
        
        // Call accessor: opcode from uVar3, width 2
        (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0), 0, local_34, uVar3, 2);
        
        if ((char)local_34[0] == '\x11') {
            // Opcode 0x11: read into local_30
            (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0), 0, &local_30, local_38[0] + 2, 2);
        }
        else if ((char)local_34[0] == '\x05') {
            // Opcode 0x05: read into local_2c
            (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0), 0, &local_2c, local_38[0] + 2, 2);
        }
        
        // Advance cursor: high byte becomes next opcode
        uVar3 = local_34[0] >> 8;
        local_38[0] = (byte)(local_34[0] >> 8);
        
        // Loop limit: 10 iterations
        if (10 < uVar10) {
            uVar5 = 0;
        }
    } while ((char)uVar5 != '\0');
    
    // Check capability bits
    if (((local_30 & 0x8000) != 0) && ((local_2c & 1) == 0)) {
        uVar2 = 1;  // Set BAR type flag
    }
}
```

**Key Points**:
* The accessor is called with opcodes that advance through the descriptor blob
* Opcode 0x11 reads 16-bit values into `local_30`
* Opcode 0x05 reads 16-bit values into `local_2c`
* The accessor uses the high byte of returned values as the next opcode (cursor)
* BAR type is determined by checking `local_30 & 0x8000` and `local_2c & 1`
* This suggests device 0xb000 might need `queue_state = 2` to trigger this path
