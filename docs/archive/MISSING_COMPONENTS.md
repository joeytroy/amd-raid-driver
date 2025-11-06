# Missing Components for Working Driver

Based on analysis of the codebase against `trx50_hw_notes.md` and the development rules, these are the critical missing pieces:

## Critical Missing Components

### 1. Firmware Capability Parsing (FUN_140007d40 equivalent)
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: CRITICAL  
**Location**: Should be in `rc_hw.c` or new `rc_firmware.c`

**What's Missing**:
- Parse firmware ASCII/Unicode capability blob from PCI config or MMIO
- Store four 16-bit values at `devExt+0x16056/58/5A/5C` (controller variants)
- Set queue variant flags (`devExt+0x16068`, `+0x1606C`)
- Detect controller type (NVMe vs AHCI) by matching vendor strings ("NVME", etc.)
- Assign callback table at `+0x16100`…`+0x16168` based on detected type
- Populate `devExt+0x1C2D8` (packed firmware capability word)

**Required Implementation**:
```c
int rc_parse_firmware_capabilities(struct rc_adapter *adapter);
// Should:
// 1. Read firmware capability blob (PCI config space or MMIO)
// 2. Parse ASCII/Unicode strings
// 3. Match against known controller types (NVMe, AHCI, etc.)
// 4. Set adapter->ctx.doorbell.variant[0-3]
// 5. Set adapter->ctx.doorbell.queue_state and queue_mode
// 6. Install appropriate callback handlers
```

### 2. Descriptor Accessor (devExt+0x1C2D0)
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: CRITICAL  
**Location**: Should be in `rc_firmware.c`

**What's Missing**:
- Function pointer at `devExt+0x1C2D0` that walks descriptor blob `DAT_140012258`
- Implements opcodes `0x05`, `0x10`, `0x11`
- Returns vendor capability words, queue depth/width pairs, feature bits
- Used by `FUN_140007d40` to populate `devExt+0x1C2D8`

**Required Implementation**:
```c
int rc_descriptor_accessor(struct rc_adapter *adapter, u32 flags, 
                           void *out_buf, u32 index, u32 width);
// Should:
// 1. Walk descriptor table at adapter->ctx.descriptor_table
// 2. Implement opcodes 0x05, 0x10, 0x11
// 3. Return capability words based on index/opcode
```

### 3. Controller Descriptor Table (DAT_1400140B0)
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: HIGH  
**Location**: Should be in `rc_firmware.c` or `rc_linux.h`

**What's Missing**:
- Static descriptor table with 40 entries (0x30-byte records)
- Each entry contains:
  - CLSID string (e.g., "CC_010802", 9 chars + NULL) at offset 0x00
  - Worker name pointer at offset 0x18
  - WMI dispatch helper pointer at offset 0x20
  - Revision/feature level at offset 0x29 (must be ≥4 for HMB)
  - Feature bitmask at offset 0x2C
- Used for WMI registration and controller type matching

**Required Implementation**:
```c
struct rc_controller_descriptor {
    char clsid[16];              // "CC_010802", etc.
    const char *worker_name;     // "HMBWORKITEM", etc.
    void *wmi_dispatch;          // WMI handler pointer
    u8 revision;                 // Feature level (≥4 for HMB)
    u32 feature_mask;            // Bitmask for feature gating
};
```

### 4. Vendor Mailbox Construction (FUN_140001008)
**Status**: ⚠️ PARTIALLY IMPLEMENTED (WRONG OFFSET)  
**Priority**: CRITICAL  
**Location**: `rc_queue.c` - `rc_ahci_build_mailbox()`

**Current Issue**:
- Mailbox is placed at offset 0x40, but Windows driver uses 0x10e-0x157
- Logic doesn't match Windows conditional paths
- Missing completion flags calculation
- Missing extended flags bit 0x20000

**Required Fix**:
- Move mailbox to correct offset: `table + 0x10e` (not 0x40)
- Implement all conditional paths:
  - Command descriptor path (`param_3[0] == 0xA1`)
  - Command payload path (`param_4[0] == 0x34`)
  - Fallback path
- Calculate completion flags based on control bits:
  - `0x4400` if `param_2[0x111] & 0x20`
  - `0x1100` if `param_2[0x111] & 0x01` and `param_2[0x113] & 0x40`
  - `0x1400` if `param_2[0x111] & 0x01` and `param_2[0x113] & 0x10`
  - `0x4703` if `param_2[0x111] & 0x01` and (`param_2[0x113] & 0x04` or `param_2[0x113] < 0`)
- Set extended flags: `table[0x154] |= 0x20000` when `param_5 == true && param_6 == false`

### 5. Doorbell Page Mapping (devExt+0x16020)
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: HIGH  
**Location**: `rc_bottom.c` or `rc_queue.c`

**What's Missing**:
- Doorbell page is never mapped/initialized
- `adapter->ctx.doorbell.doorbell_page` is NULL
- Should be mapped from a BAR or allocated as separate MMIO region

**Required Implementation**:
```c
// In rc_bottom_map_bars() or rc_queue_init():
// Map doorbell page - could be:
// 1. Separate BAR window
// 2. Offset within existing BAR
// 3. Allocated via pci_iomap()
adapter->ctx.doorbell.doorbell_page = pci_iomap(adapter->pdev, doorbell_bar, doorbell_len);
```

### 6. Callback Slot Initialization
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: HIGH  
**Location**: `rc_hw.c` or new `rc_callbacks.c`

**What's Missing**:
- Callback slots at `+0x16100`, `+0x16108`, `+0x16110`, `+0x16120`, `+0x16130`, `+0x16140`, `+0x16148` are not initialized
- Windows driver installs different handlers based on controller variant:
  - Fast-path callbacks for NVMe controllers
  - Safe dispatcher (`FUN_1400102D8`) for legacy/AHCI
- Queue enable/disable/resume handlers

**Required Implementation**:
```c
// Add to rc_linux.h:
struct rc_adapter_callbacks {
    void (*queue_dispatcher)(struct rc_adapter *adapter, ...);      // +0x16100
    void (*queue_toggle)(struct rc_adapter *adapter, ...);          // +0x16108
    void (*spinlock_callback)(struct rc_adapter *adapter);          // +0x16110
    void (*port_disable)(struct rc_adapter *adapter, ...);          // +0x16120
    void (*port_resume)(struct rc_adapter *adapter, ...);           // +0x16130
    void (*status_poll)(struct rc_adapter *adapter, ...);           // +0x16140
    void (*secondary_queue)(struct rc_adapter *adapter, ...);       // +0x16148
};

// Install based on firmware capability detection:
void rc_install_callbacks(struct rc_adapter *adapter, bool fast_path);
```

### 7. Queue Mode Selection
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: HIGH  
**Location**: `rc_queue.c` or `rc_hw.c`

**What's Missing**:
- Queue mode byte (`devExt+0x1606C`) is never set
- Should be set based on firmware capability parsing:
  - Bit 0 selects fast-path vs safe dispatcher
  - `devExt+0x16068` should be set to `0x63` + increment for NVMe

**Current State**: These fields exist in `rc_doorbell_state` but are never populated.

### 8. WMI Registration (Optional but Recommended)
**Status**: ❌ NOT IMPLEMENTED  
**Priority**: MEDIUM  
**Location**: New `rc_wmi.c`

**What's Missing**:
- WMI GUID binder (`FUN_14000a430`)
- WMI query dispatcher (`FUN_140009804`)
- WMI set/control dispatcher (`FUN_1400093f0`)
- WMI descriptor enumerator (`FUN_14000a124`)

**Note**: This is optional for basic functionality but required for full Windows compatibility.

### 9. Real Doorbell Register Addresses
**Status**: ⚠️ UNCLEAR  
**Priority**: CRITICAL  
**Location**: `rc_queue.c` - `rc_ring_doorbell()`

**Current Issue**:
- Doorbell writes use `base + RC_REG_DOORBELL + (doorbell_index * 4)`
- But Windows driver uses `devExt+0x16020` (doorbell_page) with service slot `+0x188`
- Need to verify actual hardware register addresses

**Required**:
- Test on real hardware to determine correct doorbell register offsets
- May need to use different MMIO region than base BAR

### 10. Completion Queue Processing Fix
**Status**: ⚠️ PARTIALLY IMPLEMENTED  
**Priority**: HIGH  
**Location**: `rc_hw.c` - `rc_hw_complete_locked()`

**Current Issue**:
- Completion processing exists but may not match Windows behavior exactly
- Windows driver uses service slot `+0x680` (completion pump)
- Need to verify completion queue structure matches firmware expectations

### 11. Real Hardware Register Definitions
**Status**: ⚠️ PARTIALLY IMPLEMENTED  
**Priority**: CRITICAL  
**Location**: `rc_linux.h`

**Current State**:
- Register offsets are defined but may not match actual hardware
- Need verification from hardware testing:
  - MMIO base address: 0x81a80000 (confirmed)
  - MSI vector: 244 (confirmed)
  - BAR 5 mapping: 1024 bytes (confirmed)
  - But register offsets need hardware verification

## Implementation Priority

### Phase 1: Critical for Basic Functionality
1. ✅ Fix vendor mailbox construction (offset + logic)
2. ✅ Implement firmware capability parsing
3. ✅ Map doorbell page properly
4. ✅ Initialize callback slots (at least safe dispatcher)

### Phase 2: Required for Full Functionality
5. ✅ Implement descriptor accessor
6. ✅ Create controller descriptor table
7. ✅ Set queue mode based on firmware detection
8. ✅ Verify doorbell register addresses on hardware

### Phase 3: Optional Enhancements
9. ⚠️ WMI registration (optional)
10. ⚠️ Completion queue processing refinement
11. ⚠️ Real hardware register verification

## Testing Requirements

After implementing Phase 1 components:
1. Test firmware capability parsing on real hardware
2. Verify doorbell activation sequence
3. Test vendor command submission (RC_CMD_SCAN_DISKS)
4. Verify completion processing works correctly

## Notes

- The Windows driver initialization sequence is: `rcbottom` → `rccfg` → `rcraid`
- Our Linux driver follows this but may be missing critical initialization steps
- The descriptor table and accessor are critical for firmware communication
- All hardware register addresses must be verified on actual TRX50 hardware

