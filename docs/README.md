# AMD RAID Driver Port — Documentation

Linux port of the AMD-RAID Windows driver, targeting the TRX50 chipset's
**RAID Bottom Device** (PCI `1022:B000`, NVMe controller class `0x010802`).

## The four documents

| Doc | What's in it |
|---|---|
| **[STATUS.md](STATUS.md)** | Where the port stands today, the dated implementation log of how each piece was built, and the error-handling / reset design. |
| **[IMPLEMENTATION.MD](IMPLEMENTATION.MD)** | Prioritized checklist toward a daily-driver release — blockers, missing features, polish, performance. |
| **[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)** | Authoritative RE reference: what the Windows driver actually does (AHCI-vs-nvme.c split, queue/geometry layout, debunked theories), the 9.3.2-vs-9.3.3 delta, and open questions. |
| **[archive/](archive/)** | Historical docs, including the 2024 analysis that predates the corrected findings. **Contains debunked claims** — see `archive/README.md` first. |

New here? Read **REVERSE_ENGINEERING.md** for how the hardware works, then
**STATUS.md** for what's implemented.

## Repo layout

| Path | What's there |
|---|---|
| `rc_*.c` / `rc_linux.h` | Linux driver source |
| `drivers/windows/trx50/9.3.2-00255/` | Windows driver, last vulnerable release (matches the RE docs) |
| `drivers/windows/trx50/9.3.3-00291/` | Windows driver, current build (CVE-2024-21962 fixed in `rccfg.sys`) |
| `scripts/ghidra/` | Headless Ghidra scripts that produced the `ghidra_output/` extracts |
| `docs/ghidra_output/` | Raw Ghidra extracts (decomp, call-site buckets, hex dumps) |

## Running Ghidra again

The full command lines (headless re-import + hunt, and PyGhidra targeted
decompiles) live in **[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)** under
"Reproducing the analysis".
