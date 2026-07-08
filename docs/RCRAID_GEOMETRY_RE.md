# rcraid.sys geometry parsing — reverse-engineering ground truth

This documents the on-disk RAID-metadata layout and geometry-derivation logic as
**verified against the Windows kernel driver `rcraid.sys` 9.3.2-00255** — the
exact-version match to this Linux driver (`RC_DRIVER_VERSION "9.3.2.00255"`). It
exists to replace the *inference* the Linux port was built on ("mirror Windows
bit-for-bit", "this is a guess at member position") with confirmed fact.

**Bottom line:** the Linux driver's RAID0 geometry parsing is **correct** — the
chunk-size mapping, the key struct offsets, and the RAID-level magic all match
the Windows binary. One field (member count) has a naming/offset question worth
verifying. Details below.

## Methodology

- Binary: `drivers/windows/trx50/9.3.2-00255/rcraid/rcraid.sys` (PE32+ x86-64,
  548 KB). No PDB, but the driver is rich in `RC_*` debug-format strings, so
  functions were named by their string cross-references.
- Tool: **Ghidra 12.1.2** (prebuilt release) headless + **PyGhidra** (CPython +
  JPype), OpenJDK 21. Auto-analysis + decompiler export.
- Reproduce: analyze the `.sys`, then decompile by string-xref name or address.
  Core PyGhidra driver (run with the pyghidra venv, `GHIDRA_INSTALL_DIR` set):

  ```python
  import pyghidra; pyghidra.start()
  from ghidra.app.decompiler import DecompInterface
  from ghidra.util.task import ConsoleTaskMonitor
  with pyghidra.open_program(SYS, analyze=True) as api:
      prog = api.getCurrentProgram()
      dec = DecompInterface(); dec.openProgram(prog)
      f = prog.getFunctionManager().getFunctionContaining(
              prog.getAddressFactory().getDefaultAddressSpace().getAddress(0x1400121d0))
      print(dec.decompileFunction(f, 60, ConsoleTaskMonitor()).getDecompiledFunction().getC())
  ```

## The parse flow (functions, with addresses)

| Function | Addr | Role |
|---|---|---|
| `RC_ReadMetaData` | `0x14001fd60` | reads the config packet off each member |
| `RC_BuildConfigMetadataFromRing` | `0x140019fb8` | walks the config ring: PhysicalDevice records (magic `0x25bc`) then LogicalDevice records (magic `0x25bd`) |
| `FUN_140018444` | `0x140018444` | **copies the on-disk LD packet → in-memory LD struct** (the field-offset source of truth) |
| `FUN_14001c674` | `0x14001c674` | dispatches on DeviceType → installs the per-RAID-level I/O handlers |
| `FUN_1400121d0` | `0x1400121d0` | **chunk_index → stripe-size mapping** (256K/128K/64K) |

`RC_BuildConfigMetadataFromRing` confirms the record framing the Linux driver
assumes: records are tag-prefixed `u32`s — `0x25bc` = `RC_DST_PHYSICAL_DEVICE`,
`0x25bd` = `RC_DST_LOGICAL_DEVICE` — walked until the tag no longer matches.

## On-disk LogicalDevice packet layout (verified)

From `FUN_140018444` (`param_2` = on-disk packet, `param_1` = in-memory LD):

| Field | On-disk offset | Linux `#define` | Status |
|---|---|---|---|
| record magic | `+0x00` = `0x25BD` | `RC_DST_LOGICAL_DEVICE` | ✅ |
| DeviceType | `+0x0C` | `RC_LD_DEVICETYPE_OFFSET 0x0C` | ✅ |
| element-array offset | `+0x04` | `RC_LD_ELEMENTOFFSET_OFFSET 0x04` | ✅ |
| Capacity (u64) | `+0x50` | `RC_LD_CAPACITY_OFFSET 0x50` | ✅ |
| DEVICES | `+0x68` | `RC_LD_DEVICES_OFFSET 0x68` | ⚠️ see below |
| (FirstCount) | `+0x6C` | `RC_LD_FIRSTCOUNT_OFFSET 0x6C` | ✅ copied to in-mem `[0x2d]` |
| (SecondCount) | `+0x70` | `RC_LD_SECONDCOUNT_OFFSET 0x70` | ⚠️ see below |
| PacketSize | `+0x90` | `RC_LD_PACKETSIZE_OFFSET 0x90` | ✅ (`piVar13[0x24]` sanity-check) |
| ChunkSize (u32, sectors; 0 for RAID0) | `+0xAC` | `RC_LD_CHUNKSIZE_OFFSET 0xAC` | ✅ |
| chunk_index (u32) | `+0x110` | `RC_LD_CHUNKINDEX_OFFSET 0x110` | ✅ |

`FUN_140018444` also shows **chunk_index defaults to 1 when the on-disk field is
0** (`iVar1 = *(param_2+0x110); if (iVar1==0) iVar1=1;`).

LogicalElement (per-member) offsets used by the Linux parser (`RC_LE_*`) were not
re-derived field-by-field here but are consistent with the element walk in
`RC_BuildConfigMetadataFromRing`; `UserDataOffset` at LE `+0x20` is the value the
Linux driver uses as each member's physical data offset (`ld_userdata_offset`),
which matches the assembled array on the workstation (phys_offset = 1069056
sectors).

## chunk_index → stripe size (VERIFIED — this was the main "guess")

`FUN_1400121d0` maps the in-memory chunk_index (`piVar2[0x32]`, ← on-disk
`+0x110`) to a stripe size in **512-byte sectors**:

```c
if (chunk_index == 3)      size = 0x200;   // 512 sectors = 256 KiB
else { size = 0x80;                        // 128 sectors =  64 KiB  (default)
       if (chunk_index == 2) size = 0x100; } //256 sectors = 128 KiB
```

A second site expresses the same thing as a byte-size exponent:
`0x12`→256 KiB, `0x11`→128 KiB, `0x10`→64 KiB.

This is **identical** to the Linux driver's `rc_volume_chunk_sectors_for`:

```c
case 3:  return 512u;   // 0x200  ✓
case 2:  return 256u;   // 0x100  ✓
case 1:
default: return 128u;   // 0x80   ✓
```

**Conclusion: the mapping is confirmed correct, including the fall-through.**
Windows genuinely handles only `2` and `3` explicitly; every other value
(including `1` and `0`→`1`) falls to 64 KiB. There is **no** index ≥ 4 handling
in this version — see the latent gap below.

## DeviceType → RAID level (verified)

`FUN_14001c674` switches on DeviceType (in-mem `+4` ← on-disk `+0x0C`) to install
the I/O handler pair (stored at in-mem `+0x158` / `+0x168`):

| DeviceType | RAID level | Linux `#define` |
|---|---|---|
| `0x1BF5` | (RAID-related; not in Linux enum) | — |
| `0x1BF6` | RAID0 | `RC_LDT_RAID0` ✅ |
| `0x1BF7` | RAID1 | `RC_LDT_RAID1` ✅ |
| `0x1BF9` | (variant) | — |
| `0x1BFA` | RAID5 | `RC_LDT_RAID5` ✅ |

## Cross-check summary vs the Linux driver

**Confirmed correct (upgrade the code comments from "guess" to "verified"):**
- Record magics `0x25BC` / `0x25BD`, tag-walk framing.
- DeviceType `0x0C`, RAID0 `0x1BF6` (and RAID1/RAID5).
- ChunkSize `0xAC`, chunk_index `0x110`, chunk_index default 0→1.
- chunk_index → stripe size mapping (3→256K, 2→128K, else→64K).

**Open question — member count field (`0x68` vs `0x70`):**
`FUN_140018444` copies three adjacent counts — on-disk `0x68`→in-mem `[0x2c]`,
`0x6C`→`[0x2d]`, `0x70`→`[0x2e]`. The Linux driver treats `0x68` (`DEVICES`) as
the member count. The Windows striping calc `FUN_1400121d0` uses in-mem `[0x2e]`
(← on-disk `0x70`, which Linux names `SECONDCOUNT`). For a plain 2-disk RAID0
these are equal (and the workstation array assembles correctly with 2/2 members),
so this is **not** a live bug here — but the authoritative member/element count
for exotic layouts (RAID10, spanned, RAID5 with parity) should be confirmed
before relying on it. Recommend RE'ing the element-count/validation path
(`RC_ValidateLogicalElement` @ `0x140023554`) to settle which field governs.

**Latent gap (not affecting this box):**
Neither this Windows version nor the Linux driver handles `chunk_index ≥ 4`; both
silently fall back to 64 KiB. If a newer RAIDXpert2 / firmware ever creates an
array with a 512 KiB or 1 MiB stripe encoded as index 4/5, **both** drivers would
mis-map it and corrupt data on write. The 9.3.3 `rcraid.sys` is **not** in the
tree (only `rcbottom.sys`/`rccfg.sys` are), so whether 9.3.3 extended the mapping
is unverified. The geometry-trust write-veto (see `WORKSTATION_SESSION.md` §5)
does **not** catch this, because the LD parses fine — only the stripe size is
wrong. Worth a guard: treat an unrecognized chunk_index as untrusted.

## Follow-ups
- [ ] Update `rc_nvme.c` / `rc_linux.h` comments: cite `FUN_1400121d0` /
      `FUN_140018444` and drop the "guess"/"mirror" hedging for the confirmed items.
- [ ] Settle the `0x68` vs `0x70` member-count question via the element-validation path.
- [ ] Consider flagging `chunk_index` values other than {0,1,2,3} as untrusted
      (fail closed) rather than silently using 64 KiB.
- [ ] If a 9.3.3 `rcraid.sys` becomes available, diff its chunk_index mapping.
