# Open Reversing Questions

What still needs Ghidra investigation to finish the NVMe path. Each item
includes the function or data symbol to look at and what we're trying to
learn. Run via the headless script in `scripts/ghidra/HuntBlockers.java`
(see `docs/README.md` for the command line).

## High priority — needed for the NVMe path to issue commands

### 1. `FUN_14000f608` (in `rcbottom.sys`)

Called from `FUN_14000dd44` (the NVMe queue-init callback) once per I/O
queue. We pulled out the prep code (DMA alloc, queue-handle struct
layout) in `docs/ghidra_output/rcbottom_9.3.2/key_funcs.c`, but the
actual **per-queue MMIO arming** lives in `FUN_14000f608` and we haven't
read it yet.

What we need:
- Confirm doorbell stride and base (`BAR0 + 0x1000` vs anything
  vendor-specific).
- Confirm Create-IO-CQ / Create-IO-SQ admin command shapes if AMD uses
  any non-standard fields.
- Whether the driver pre-creates a Create-IO-CQ via the admin queue or
  programs MMIO registers directly.

### 2. `rcraid.sys` NVMe completion ISR

`rcraid.sys` is byte-identical between 9.3.2 and 9.3.3, so prior docs in
`docs/archive/legacy_2024/TECHNICAL_REFERENCE.md` apply, but the
completion ISR (`FUN_14000d2b8`) was never decompiled. We need:

- Exact CQ entry layout the driver expects (16-byte NVMe-spec
  completion vs. an AMD extension).
- Phase-bit walk vs. head-pointer DMA write-back.
- How error / Identify completions are routed back to the submitter.

### 3. Per-namespace discovery in `rcraid.sys`

The Windows driver presents virtual RAID volumes, not raw NVMe
namespaces. Somewhere in `rcraid.sys` it issues Identify commands and
then assembles RAID metadata. We want:

- The vendor-specific opcode used for RAID-volume enumeration (if any),
  or confirmation that everything is built on top of standard NVMe
  Identify + Read.
- The metadata signature/version constants so `rc_metadata.c` can
  recognise the on-disk superblocks.

## Medium priority — improves the AHCI path or fills documentation gaps

### 4. Service-slot `+0x9d8` shape

We know it's called five times from `rcbottom.sys` (see
`docs/ghidra_output/rcbottom_9.3.2/indirect_call_offsets.txt`) with a
pointer to a small struct. The struct describes "program these MMIO
registers with these values". For the AHCI path it programs the
`0x20–0x38` queue base/size set; for the NVMe path it programs the
admin queue (we already do this directly in `rc_nvme_init_controller`).

We don't strictly need the implementation to proceed (we replaced it
with direct `writel` in NVMe mode), but a small Ghidra trace of
`FUN_140008a48`'s argument shape would let us write a more general
helper and remove the duplicated code in `rc_hw.c`.

### 5. `rcraid.sys` 9.3.2 vs 9.3.3 binary diff confirms no functional change

Already known to be byte-identical (SHA256 match), but a fast `bsdiff`
or `radare2 diff` against the section table would catch any future
changes if a 9.3.4 ships.

## Low priority — only if the NVMe controller refuses to come ready

### 6. `FUN_140008f34` BAR discovery

This is what runs before our entry point on Windows. Walks
`MmMapIoSpace`-ed BARs and stashes them in the device extension.
Should match what Linux's `pci_iomap` returns, but if the
`rc_nvme_init_controller` read-back fails, this is the function to
re-read.

### 7. Vendor PCI config writes before MMIO bring-up

`FUN_140005ff4` calls services `+0xa8/+0xb0/+0xb8/+0xe8/+0x108/+0x188`
before anything queue-related runs. Most are DMA setup, but `+0xe8`
(seen passing a 0x1c-byte struct with values `0x1c,4,2,2,2,2,1`) might
set adapter parameters that we currently don't replicate. Worth a look
if NVMe init produces a controller fatal status.

## Volume-metadata parsing (member count + stripe size)

The driver still hardcodes `RC_VOLUME_EXPECTED_MEMBERS = 2` and uses
`RC_MetaData.ConfigRingSize` as a proxy for stripe size.  Both happen
to be correct on the 2-member dev box but neither is properly parsed
from the on-disk metadata.  The fix requires reading the config packet
at `ConfigCommitOffset` (LBA 0x5001) and the records the packet
references in the config ring at `ConfigRingOffset` (LBA 0x5800).

### What's known

`rcblob.x86_64` in the AMD-shipped Linux SDK (path:
`drivers/linux/wrx80/raid_linux_driver_930_00283/.../driver_sdk/src/`)
is **ELF with full DWARF debug info**. Use `pahole -C <struct>` to
extract struct layouts. Key types:

- `RC_MetaData` (512 B) — the LBA 0x5000 block. Layout fully known
  and mirrored in `struct rc_raidcore_md` in `rc_linux.h`.
- `RC_ConfigPacketHeader` (512 B, packed) — the config packet
  header at `ConfigCommitOffset`. Has `PacketSize`, `Version`,
  `Physical/Logical/Controller/SEP DeviceOffset+Size` field pairs.
  `RC_BuildConfigMetadataFromRing` treats `*Offset` as a byte offset
  into a `PacketSize`-byte buffer and expects signature `0x25bc` at
  that location.
- `RC_LogicalDevice` (516 B, packed) — contains the fields we need:
  **`Devices` (u32 @ offset 104)** = member count, **`ChunkSize`
  (u32 @ offset 172)** = stripe size.
- `RC_LogicalElement_LE` (64 B) — per-member record inside a
  logical device, has the linked physical-device `ID` so we can
  match members → positions.
- `RC_PhysicalDeviceOnDisk` (128 B, packed) — per-physical-disk
  on-disk record.

### What blocks parsing right now

Our dev-box LBA 0x5001 dump has `PacketSize=302`,
`PhysicalDeviceOffset=34192`.  Per the SDK function the offset must
be within the packet (i.e. < PacketSize), so the SDK code would
reject our packet outright.  Most likely the on-disk layout drifted
between SDK 9.3.0 (the open source we have) and the Windows version
that wrote our array (probably 9.3.2+).  Without a matching SDK we'd
need to reverse the Windows `rcraid.sys` writer, or download a newer
RHEL SDK build, to confirm the current layout.

### Next steps when this is picked up

1. Look for newer AMD Linux SDK builds (9.3.2-00255 to match
   `drivers/windows/trx50/9.3.2-00255/`).  Diff the relevant struct
   sizes via `pahole`.
2. If no newer SDK is available, disassemble `RC_DiskMetaDataIO` /
   `RC_BuildConfigMetadataFromRing` end-to-end and rebuild the
   on-disk layout from the field access patterns.  Tools are
   simple: `nm -S`, `objdump -d -M intel --disassemble=<sym>`,
   `pahole -C <struct>`.
3. Once layout is known, parse member count and stripe at probe
   time and replace `RC_VOLUME_EXPECTED_MEMBERS` + the
   ConfigRingSize-as-stripe workaround.

## Closed / debunked (do not re-investigate)

Each of these wasted significant effort in the earlier round of
analysis. Do not chase them again:

- **Descriptor accessor at `devExt+0x1C2D0`** — it's the WDF/KMDF
  class-bind function pointer installed by service `+0x418`. Not a
  firmware-capability accessor.
- **`DAT_140012258` "firmware capability blob"** — it's the WDF
  version-bind table (GUIDs + KMDF strings). Confirmed by hex dump in
  `docs/ghidra_output/rcbottom_9.3.2/firmware_descriptor_blob.txt`.
- **`FUN_14002ce29`** — outside `rcbottom.sys` (binary ends near
  `0x14001a700`). Was speculative; the function does not exist in this
  binary.
- **"40-entry controller descriptor table"** — never existed.
- **Completion register writes at `BAR0 + 0x100 + qid*0x10`** — those
  offsets are AHCI port registers, not NVMe completion registers.
  Wrong for DEV_B000 by construction.
