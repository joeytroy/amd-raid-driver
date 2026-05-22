# Archived Documentation

Historical documentation kept for reference. **None of this is the
current state of the port.** See `docs/README.md` for live documentation.

## `legacy_2024/` — superseded by GHIDRA_FINDINGS_2026

The three documents in `legacy_2024/` (`TECHNICAL_REFERENCE.md`,
`IMPLEMENTATION_STATUS.md`, `GHIDRA_ANALYSIS_NEEDED.md`) were the primary
analysis docs through 2024. They contain significant amounts of
**incorrect** material:

- They treat the controller as the AHCI variant, when in fact PCI
  `1022:B000` takes the Windows driver's `nvme.c` path.
- They describe a "firmware capability descriptor accessor" at
  `devExt+0x1C2D0` reading a blob `DAT_140012258`. Both turned out to
  be the WDF/KMDF version-bind machinery, not a firmware capability
  interface.
- They list `FUN_14002ce29` as a candidate descriptor accessor; that
  address is outside `rcbottom.sys`.

The function-level analysis of `rcraid.sys` (command submission,
completion callbacks, queue dispatchers) is still largely valid since
that binary is byte-identical between 9.3.2 and 9.3.3.

For the corrected picture, read **`docs/GHIDRA_FINDINGS_2026.md`** at
the top level.

## Older standalone files

- **`DEVELOPMENT_ASSESSMENT.md`** — early go/no-go feasibility writeup.
- **`IMPLEMENTABILITY_ANALYSIS.md`** — predecessor to the status doc.
- **`IMPLEMENTATION_SUMMARY.md`** — earlier consolidation attempt.
- **`MISSING_COMPONENTS.md`** — listed the "descriptor accessor",
  "controller descriptor table" etc. that we now know don't exist as
  described.
