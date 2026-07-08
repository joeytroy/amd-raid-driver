# rcraid.sys in 9.3.3-00291

This is the genuine 9.3.3-00291 `rcraid.sys`, extracted from AMD's
`raid_windows_driver_933_00291.zip` (Win11/x64/NVMe_DID). It is **NOT**
byte-identical to the 9.3.2-00255 copy — an earlier note assumed it was,
before the 9.3.3 binary was actually obtained:

  - 9.3.2-00255: SHA256 `f0a6fc8b…`, 560,576 bytes
  - 9.3.3-00291: SHA256 `3f241608…`, 563,528 bytes (+2,952)

The **geometry parsing is unchanged**, though — verified by re-running the
Ghidra pipeline on this binary (`docs/RCRAID_GEOMETRY_RE.md`): identical
chunk_index→stripe mapping, on-disk field offsets, and RAID-level magics.
The 2,952-byte difference is elsewhere (added RAID10 paths / the CVE-era
changes), not in the RAID0 metadata parser the Linux port mirrors.
