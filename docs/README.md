# AMD RAID Driver Port — Documentation Index

Linux port of the AMD-RAID Windows driver, targeting the TRX50 chipset's
**RAID Bottom Device** (PCI `1022:B000`, NVMe controller class `0x010802`).

## Read these first

1. **[GHIDRA_FINDINGS_2026.md](GHIDRA_FINDINGS_2026.md)** — Authoritative
   technical analysis. Documents what the Windows driver actually does for
   device `0xB000` (it takes the `nvme.c` code path, not AHCI) and why the
   earlier "descriptor accessor / `DAT_140012258` blob" theory was wrong.
2. **[STATUS.md](STATUS.md)** — Where the Linux port stands today.
3. **[OPEN_QUESTIONS.md](OPEN_QUESTIONS.md)** — What still needs reversing
   in Ghidra before the next big push.

## Repo layout

| Path                                 | What's there                                                          |
|--------------------------------------|-----------------------------------------------------------------------|
| `rc_*.c` / `rc_linux.h`              | Linux driver source                                                   |
| `drivers/windows/trx50/9.3.2-00255/` | Windows driver, last vulnerable release (matches analysis docs)       |
| `drivers/windows/trx50/9.3.3-00291/` | Windows driver, current build (CVE-2024-21962 fixed in `rccfg.sys`)   |
| `scripts/ghidra/HuntBlockers.java`   | Headless Ghidra script that produced the findings under `ghidra_output/` |
| `docs/ghidra_output/`                | Raw Ghidra extracts (decomp, call-site buckets, hex dumps)            |
| `docs/archive/legacy_2024/`          | Earlier analysis docs. **Contains debunked claims** — see `legacy_2024/README.md` before using. |

## Running Ghidra again

```sh
# Set Java (Ghidra 12 needs JDK 17+)
export JAVA_HOME="C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot"

# Re-import (project stored outside the repo)
"C:\Program Files\ghidra_12.1_PUBLIC\support\analyzeHeadless.bat" \
  C:/Users/dev/ghidra_projects amd-raid \
  -import drivers/windows/trx50/9.3.2-00255/rcbottom/rcbottom.sys \
  -overwrite

# Re-run the hunt
"C:\Program Files\ghidra_12.1_PUBLIC\support\analyzeHeadless.bat" \
  C:/Users/dev/ghidra_projects amd-raid \
  -process rcbottom.sys -noanalysis \
  -scriptPath scripts/ghidra -postScript HuntBlockers.java \
  docs/ghidra_output/rcbottom_9.3.2
```
