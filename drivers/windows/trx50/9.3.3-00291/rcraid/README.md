# rcraid.sys in 9.3.3-00291

This release ships the **byte-identical** `rcraid.sys` from the
9.3.2-00255 release (SHA256 match documented in
`docs/GHIDRA_FINDINGS_2026.md`). To avoid duplicating a 547 KiB
binary the repo keeps a single copy in the 9.3.2 tree:

  `drivers/windows/trx50/9.3.2-00255/rcraid/rcraid.sys`

If a future AMD release changes `rcraid.sys`, add the new copy here
and update the diff note in `GHIDRA_FINDINGS_2026.md`.
