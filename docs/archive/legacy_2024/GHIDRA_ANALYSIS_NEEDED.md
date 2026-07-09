# Ghidra Analysis Needed for TRX50 Driver — ARCHIVED

> **⚠️ Archived 2026-05-22.** Most of the "TO DO" items in this document
> have either been answered (and the answers contradicted the
> hypothesis) or were chasing things that don't exist:
> - Hunting for `0xB000` register offsets — the controller uses NVMe
>   register layout (`BAR0 + 0x14/0x24/0x28/0x30`), not the AHCI
>   `0x100+` offsets.
> - Hunting for the descriptor accessor at `devExt+0x1C2D0` — it's the
>   WDF/KMDF version-bind function pointer, not a firmware accessor.
> - Hunting for `FUN_14002ce29` — outside the binary; does not exist.
>
> Current Ghidra TODOs live in **`docs/REVERSE_ENGINEERING.md`** at the repo
> top level. Keep this archived file only as a record of what we used
> to think.

**Note**: All analysis results are documented in `TECHNICAL_REFERENCE.md`. Use function addresses from that file as starting points in Ghidra.

**Ghidra Tips**:
- The **Listing view** is better for precise register offsets and MMIO addresses
- The **Decompiler view** is better for understanding logic flow
- Use function addresses from `TECHNICAL_REFERENCE.md` as starting points

---

## 🔴 TO DO - Critical Analysis Needed

### 📁 `rcbottom.sys` - Hardware Initialization Layer

**File**: `windows/rcbottom/rcbottom.sys`  
**Priority**: HIGH - Hardware initialization and register programming

#### Priority 1: Register Offsets for Device 0xb000 (URGENT)

**Problem**: Command and completion queue register writes aren't persisting for device 0xb000.

**In Ghidra (`rcbottom.sys`), look for:**

- [ ] **Device ID constant**: Search for `0xb000` or `0xB000` (hex constant)
- [ ] **Device ID comparisons**: `cmp word ptr [rcx+0x?], 0xb000` or `movzx eax, word ptr [rcx+0x?]` followed by `cmp eax, 0xb000`
- [ ] **Register offset tables**: Find if device 0xb000 uses different register offsets than 0x43bd
- [ ] **Command queue base low**: `mov [rbx+0x20],` or `mov dword ptr [rbx+0x20],` (offset 0x020)
- [ ] **Command queue base high**: `mov [rbx+0x24],` (offset 0x024)
- [ ] **Command queue size**: `mov [rbx+0x28],` (offset 0x028)
- [ ] **Completion queue base (working)**: `mov [rbx+0x30],` (offset 0x30 - this one works!)
- [ ] **Completion registers (not working)**: `mov [rbx+0x100],` or `mov dword ptr [rbx+0x100],` (check if used for 0xb000)
- [ ] **Register write functions**: `writel` or `WRITE_REGISTER_ULONG` calls

**Key questions to answer:**
- Does device 0xb000 use different register offsets than 0x43bd?
- Why does offset 0x30 work but 0x100+ doesn't?
- What is the exact register layout for device 0xb000?

#### Priority 2: Service Slot +0x188 Implementation (Doorbell Writes)

**Problem**: Doorbell order is correct (1, 4, 2, 3), but we need to verify the exact MMIO register being written.

**In Ghidra (`rcbottom.sys`), look for:**

- [ ] **Service slot `+0x188`**: Find where StorPort service table `+0x188` points to (may be in StorPort library, but check `rcbottom.sys` first)
- [ ] **Doorbell MMIO writes**: `mov [rax+0x10],` or similar MMIO writes
- [ ] **MMIO base + offset 0x10**: Functions that write to MMIO base + offset 0x10
- [ ] **Doorbell page usage**: Check if `devExt+0x16020` (doorbell_page) is actually used
- [ ] **Device 0xb000 doorbell**: Verify exact doorbell register offset for device 0xb000

#### Priority 3: Completion Register Programming Implementation

**Problem**: Service slot `+0x9D8` call sites are documented, but the actual MMIO register writes are still unknown. Completion register writes to offset 0x100+ are not persisting for device 0xb000.

**Status**: ✅ Call sites documented | ❌ Implementation still needed

**What we know** (call sites documented in `TECHNICAL_REFERENCE.md`):
- ✅ `FUN_1400021d4` (rcbottom.sys): Calls service `+0x9D8` with `devExt + 0x15928`, `queue_desc + 0x6e8`, `queue_desc + 0x6f0` (3 times)
- ✅ `FUN_140008a48` (rcbottom.sys): Calls service `+0x9D8` with `devExt + 0x16288`, `devExt + 0x1c2e8` (2 times)
- ✅ `FUN_140007978` (rcbottom.sys): Calls service `+0x9D8` with `&DAT_140216880` (global context)

**What we need** - In Ghidra (`rcbottom.sys` or StorPort library), look for:

- [ ] **Direct MMIO writes**: `mov [rbx+0x100],` or `mov dword ptr [rbx+0x100],` in listing view
- [ ] **Completion register addressing**: `lea rdx, [rbx+0x100]` or similar
- [ ] **Per-queue offset calculation**: `offset = 0x100 + (queue_idx * 0x10)` pattern
- [ ] **Device-specific handling**: Check if device 0xb000 uses different completion register programming
- [ ] **Service slot `+0x9D8` implementation**: Find where StorPort service table `+0x9D8` points to (may be in StorPort library)

**Note**: The actual implementation of service slot `+0x9D8` is likely in StorPort library (storport.sys), but we should also check for direct MMIO writes in `rcbottom.sys` that might bypass the service call.

#### Priority 4: Firmware Capability Blob Source

**Problem**: Currently using stub defaults - need to find where firmware capability data is read from.

**In Ghidra (`rcbottom.sys`), look for:**

- [x] **Descriptor accessor usage in `FUN_140007d40`**: ✅ DOCUMENTED
  - Device-specific opcodes (0x82/0xc6 for 0x7905, 0x108/0x10a for 0x7916/0x7917)
  - Capability word construction (high/low word combination)
  - Complete descriptor accessor sequence for NVMe path (opcode 0x34 → loop until 0x10)
  - Queue configuration extraction (bits 0-3 and 4-9 from descriptor blob)
  - Link between queue configuration and `FUN_14000c1e4` control register flags

- [x] **Firmware blob source in `FUN_140007d40`**: ✅ DOCUMENTED
  - Uses StorPort service `+0x418` with static descriptor blob at `DAT_140012258`
  - Call: `service(+0x418)(StorPortContext, devExt+0x20, &DAT_140012258, devExt+0x1c298)`
  - Sets up descriptor accessor function pointer at `devExt+0x1c2d0` and context at `devExt+0x1c2a0`
  - `DAT_140012258` is a static data structure containing firmware capability information

- [ ] **Descriptor accessor initialization**: `mov [rbx+0x1c2d0],` - where descriptor accessor function pointer is set (likely in service `+0x418` implementation or StorPort library)
  - **POTENTIAL FINDING**: `0x1c2d0` may be linked to function at `14002ce29` - verify if this is the descriptor accessor implementation or where it's initialized
- [ ] **Descriptor accessor implementation**: How does `devExt+0x1C2D0` function read data? (may be in StorPort library)
  - **TARGET**: Function at `14002ce29` may be the descriptor accessor implementation - analyze this function
- [ ] **PCI config space reads**: `pci_read_config_*` or `IoReadPciConfig*` (if capability data is read from PCI config space)
- [ ] **MMIO reads from capability registers**: Memory-mapped I/O reads for capability data
- [ ] **ACPI table access**: `MmReadSystemMemory` or similar memory reads for ACPI tables

#### Functions Needing Verification (from `rcbottom.sys`, but used by `rcraid.sys`)

**⚠️ IMPORTANT**: These functions were analyzed from `rcbottom.sys`, but are used by `rcraid.sys` functions. Need to verify if they exist in `rcraid.sys` or are exported/imported:

- [x] **`FUN_14000c9e4`**: Scatter-gather list builder (pre-submission) ✅ DOCUMENTED (from `rcbottom.sys`)
  - Used by: `FUN_14000e2c8`, `FUN_14000ea34`, `FUN_14000ff50` (all in `rcraid.sys`)
  - Purpose: Builds scatter-gather list for DMA data transfer before command submission
  - Processes SGL entries, calculates page alignment, builds descriptor array
  - **TODO**: Verify if this function exists in `rcraid.sys` or if it's exported from `rcbottom.sys`

- [x] **`FUN_14000c1e4`**: Command configuration after state machine completion ✅ DOCUMENTED (from `rcbottom.sys`)
  - Used by: `FUN_14000dae4`, `FUN_14001023c` (all in `rcraid.sys`)
  - Purpose: Configures command control register with flags, triggers command submission
  - Sets up control register at `param_1[+0x3a4]` with device-specific flags
  - Calls `FUN_14000e68c` for final command submission if conditions met
  - **TODO**: Verify if this function exists in `rcraid.sys` or if it's exported from `rcbottom.sys`

- [x] **`FUN_140005ff4`**: Adapter initialization and device enumeration ✅ DOCUMENTED (from `rcbottom.sys`)
  - Purpose: Main adapter initialization function
  - Sets up adapter contexts, parses device information, initializes firmware capabilities
  - Sets `devExt+0x16020` from `devExt+0x16018` in `FUN_1400067fc`
  - Handles multi-adapter linkage

- [x] **`FUN_1400067fc`**: Adapter object & WMI registration helper ✅ DOCUMENTED (from `rcbottom.sys`)
  - Purpose: Completes adapter setup after initial context creation
  - Sets `devExt+0x16020` from `devExt+0x16018` after service `+0x258` call
  - Establishes adapter linkage and WMI registration

- [x] **`FUN_140006e3c`**: Completion processing and adapter iteration ✅ DOCUMENTED (from `rcbottom.sys`)
  - Purpose: Processes queue completions from all adapters
  - Iterates adapters using `devExt+0x16020` via adapter list context
  - Builds completion descriptors for WMI/StorPort

- [x] **`FUN_14000a564` / `FUN_14000a72c`**: Multi-adapter WMI/descriptor binding ✅ DOCUMENTED (from `rcbottom.sys`)
  - Purpose: Connect/disconnect adapters in multi-adapter scenarios
  - Uses `devExt+0x16020` for adapter iteration and context retrieval

- [x] **`FUN_14000e960`**: Queue full handler - deferred work item queue ✅ DOCUMENTED (from `rcbottom.sys`)
  - Used by: `FUN_14000fafc`, `FUN_14000d06c` (and other queue dispatchers in `rcraid.sys`)
  - Purpose: Handles queue-full conditions by queuing work items for deferred processing
  - Allocates 0x28-byte work item structure with tag `0x72634148` ("HAcr")
  - Maintains linked list of deferred SRB commands at `devExt+0x15f88` (head) and `devExt+0x15f90` (tail)
  - Protected by spinlock at `devExt+0x15f80`

- [x] **`devExt+0x16020` usage**: ✅ DOCUMENTED
  - Primary adapter/controller handle
  - Set from `devExt+0x16018` in `FUN_1400067fc`
  - Used with services `+0x650`, `+0x188`, `+0x6d8`, `+0x680`
  - Used for adapter traversal in multi-adapter scenarios

---

### 📁 `rcraid.sys` - RAID Command Layer

**File**: `windows/rcraid/rcraid.sys`  
**Priority**: HIGH - Command submission and completion handling

#### Priority 1: ISR Completion Processing

**Problem**: Need to understand how completions are read from hardware queues.

**In Ghidra (`rcraid.sys`), look for:**

- [ ] **`FUN_14000d2b8`**: ISR completion processing implementation
- [ ] **Completion queue reads**: How are completion entries read from hardware?
- [ ] **Completion status checking**: How is completion status verified?
- [ ] **Queue pointer management**: How are head/tail pointers managed?
- [ ] **Completion descriptor parsing**: How are completion descriptors processed?

#### Priority 2: Queue Processing Functions

**In Ghidra (`rcraid.sys`), look for:**

- [ ] **`FUN_14000cb4c`**: Called for queue processing
  - Used by: `FUN_14000eef8` when completion type is 7
  - Purpose: Queue processing logic
  - **Note**: This function is called when completion type is 7, so it likely processes queue entries

---

### 📁 StorPort Library / External Dependencies

**Priority**: MEDIUM - May require analyzing Windows kernel/StorPort library

#### Service Slot Implementations

**In Ghidra (StorPort library or Windows kernel), look for:**

- [ ] **Service slot `+0x188`**: Find where StorPort service table `+0x188` points to (doorbell writes)
- [ ] **Service slot `+0x9D8`**: Find where StorPort service table `+0x9D8` points to (completion register programming)
- [ ] **Service slot `+0x418`**: Find where StorPort service table `+0x418` points to (descriptor accessor initialization)

**Note**: These may be in Windows kernel (storport.sys) or part of the StorPort framework. The call sites are documented in `TECHNICAL_REFERENCE.md`, but the actual implementations are in StorPort library.

---

## ✅ COMPLETED - Analysis Done

### Functions Fully Analyzed

**From `rcbottom.sys`**:
- ✅ `FUN_140008f34` - BAR discovery and queue setup (stack layout, `MmMapIoSpace` usage)
- ✅ `FUN_140008638` - Doorbell activation caller (stack layout, call flow)
- ✅ `FUN_1400079a4` - Port registration (stack layout, callback installation)
- ✅ `FUN_14000924c` - Doorbell activation (stack layout, `KeStallExecutionProcessor` usage, call relationships)
- ✅ `FUN_140001ed8` - Legacy queue bring-up helper (stack layout)
- ✅ `FUN_14000a564` - WMI/descriptor binder (stack layout, multi-adapter disconnect)
- ✅ `FUN_140007d40` - Firmware capability parsing (COMPLETE: all 23+ callback implementations, device ID matching, queue state paths)
- ✅ `FUN_140008a48` - Queue initialization (complete implementation, service slot +0x9D8 call sites)
- ✅ `FUN_140007978` - Global completion register programming
- ✅ `FUN_1400093c4` - Queue state table lookup
- ✅ `FUN_14000273c` - Error logging
- ✅ `FUN_140003d94` - AHCI command submission
- ✅ `FUN_140004170` - Command routing helper
- ✅ `FUN_1400075ac` - Command submission callback
- ✅ `FUN_14000f178` - NVMe command submission
- ✅ `FUN_14000f454` - NVMe queue initialization
- ✅ `FUN_140010488` - MMIO register I/O (offset 0x10, not 0x100+)
- ✅ `FUN_140008bc0` - Descriptor/WMI registration and cleanup
- ✅ `FUN_1400043e0` - Queue validation/iteration
- ✅ `FUN_140011140` - memcpy/memmove implementation
- ✅ `FUN_140001008` - Vendor mailbox construction
- ✅ `FUN_140005ff4` - Adapter initialization and device enumeration
- ✅ `FUN_1400067fc` - Adapter object & WMI registration helper
- ✅ `FUN_140006e3c` - Completion processing and adapter iteration
- ✅ `FUN_14000a72c` - Multi-adapter WMI/descriptor binding (connect)
- ✅ `FUN_14000e960` - Queue full handler (deferred work item queue)

**From `rcraid.sys`**:
- ✅ `FUN_14000d29c` - ISR resource cleanup (DMA/spinlocks)
- ✅ `FUN_14000c2fc` - Descriptor lookup (core NVMe command slot allocation, queue structure, descriptor slot structure)
- ✅ `FUN_14000c900` - SRB completion/error handler (complete implementation)
- ✅ 20+ NVMe command submission functions - All documented (use `FUN_14000c2fc` pattern)
- ✅ Completion callbacks - `FUN_14000e1ec`, `FUN_14001005c`, `FUN_140010184`, `FUN_1400097ac`
- ✅ Queue callback handler - `FUN_14000eef8`
- ✅ Command routing functions - `FUN_14000fafc`, `FUN_14000ed2c`, `FUN_14000ec64`, `FUN_14001026c`
- ✅ Special command handlers - `FUN_14000f838`, `FUN_14000fa2c`
- ✅ Scatter-gather list builder - `FUN_14000c9e4` (pre-submission SGL processing, from `rcbottom.sys`)
- ✅ Command configuration - `FUN_14000c1e4` (state machine completion, control register setup, from `rcbottom.sys`)

### Features Implemented

- ✅ Doorbell order (1, 4, 2, 3) - FIXED in Linux driver
- ✅ BAR discovery logic - documented with stack layouts
- ✅ Port registration logic - documented with stack layouts
- ✅ Vendor mailbox construction - documented
- ✅ Global constants (`PTR_DAT_140014090` / `DAT_140014078 = 0x28`) - documented
- ✅ `KeStallExecutionProcessor` usage - call sites identified (5µs and 25µs stalls)
- ✅ Service slot +0x9D8 call sites - 5+ locations documented
- ✅ Command submission flow - complete (20+ functions documented)
- ✅ Completion handling - complete (5 callback functions documented)

### Data Structures Documented

- ✅ Queue structure layout (command/completion queues)
- ✅ Descriptor slot structure (0x78 bytes)
- ✅ Device extension offsets (all callback slots documented)
- ✅ StorPort service table slots (key functions identified)
- ✅ Vendor mailbox layout (dynamic construction)
- ✅ Work item queue structure (0x28 bytes, linked list)

---

## Detailed Analysis Context

### Hardware Observations

From test results:
- **BAR0**: 16KB (0x4000) at physical address 0xaef00000 / 0xaee00000
- **Register dump** shows:
  - Offset 0x0: 0x3c01ffff (status)
  - Offset 0x4: 0x08000020 (control)  
  - Offset 0x8: 0x00020000 (interrupt status)
  - Offset 0xc: 0x0000000f (interrupt mask)
  - Offset 0x10: 0x0000000f (doorbell - all 4 bits set)
  - Offset 0x30: 0xff27c000 (completion queue base - THIS ONE WORKS!)

**Key Insight**: Offset 0x30 seems to work for completion queue base, but offsets 0x100+ don't. This suggests:
- The register layout might be different for device 0xb000
- Or completion registers are at a different base offset
- Or we need to enable something first before offsets 0x100+ work

### Windows Driver Files

**Primary Files**:
1. **`windows/rcbottom/rcbottom.sys`** - Hardware initialization layer (HIGH PRIORITY)
2. **`windows/rcraid/rcraid.sys`** - RAID command layer (HIGH PRIORITY - mostly complete)
3. **`windows/rcconfig/rccfg.sys`** - Configuration layer (LOW PRIORITY)

**Driver Version**: 9.3.2-00255 (from `rcraid.sys`)

### Function-to-File Quick Reference

| Function | File | Status |
|----------|------|--------|
| `FUN_140008f34` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_14000924c` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_140001ed8` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_14000a564` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_140007d40` | `rcbottom.sys` | ✅✅ Fully Analyzed |
| `FUN_140008a48` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_140005ff4` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_1400067fc` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_140006e3c` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_14000a72c` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_14000e960` | `rcbottom.sys` | ✅ Analyzed |
| `FUN_14000c2fc` | `rcraid.sys` | ✅ Analyzed |
| `FUN_14000c900` | `rcraid.sys` | ✅ Analyzed |
| `FUN_14000d2b8` | `rcraid.sys` | ⏳ TODO |
| `FUN_14000cb4c` | `rcraid.sys` | ⏳ TODO |
| Service slot +0x188 | StorPort / `rcbottom.sys` | ⏳ TODO |
| Service slot +0x9D8 | StorPort | ⏳ TODO (call sites documented) |
| Service slot +0x418 | StorPort | ⏳ TODO (call sites documented) |
| Descriptor accessor init | StorPort / `rcbottom.sys` | ⏳ TODO |

### Notes

- All analysis results are documented in `TECHNICAL_REFERENCE.md`
- Start with `rcbottom.sys` for register programming code
- Use function addresses from `TECHNICAL_REFERENCE.md` as starting points
- The Listing view in Ghidra is better for precise register offsets
- The Decompiler view is better for understanding logic flow
