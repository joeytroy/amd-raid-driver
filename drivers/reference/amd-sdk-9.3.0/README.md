# AMD Linux RAID Driver SDK — 9.3.0-00283

Original location on AMD's driver download page (free, no EULA):
`https://www.amd.com/en/support/` — search for "RAID Driver" for the
TRX50 / WRX80 chipsets.

## Contents

Both subdirectories contain near-identical material; AMD ships them
for two different distribution targets:

- `raid_linux_driver_930_00283/` — the RHEL DKMS package
  (`dd-rcraid-RHEL8-*`)
- `raidxpert2_linux_930_00283/` — broader package including the
  `rcadm` userspace administration tool and the AMD-RAID user guide
  PDFs

The kernel-source SDK is at
`*/driver_sdk/src/`:

| File | License | Use here |
|------|---------|----------|
| `rc_init.c`, `rc_config.c`, `rc_msg.c`, `rc_mem_ops.c`, `rc_event.c` | GPL-2 per AMD headers | **Reference only** — read for API understanding. No code copied into this project. |
| `rc.h`, `rc_adapter.h`, `rc_srb.h`, `rc_scsi.h`, `rc_ahci.h`, `rc_msg_platform.h`, `rc_types_platform.h`, `rc_pci_ids.h` | GPL-2 per AMD headers | **Reference only** — same as above. |
| `rcblob.x86_64` | AMD `LICENSE_SDK` (proprietary) | **Cross-reference only** — ELF with DWARF debug info, used via `pahole`/`objdump` to confirm struct layouts derived from disassembly of the Windows `.sys` binaries. Never linked, never copied. |
| `LICENSE_SDK` | AMD proprietary | Documents the restrictions AMD places on the SDK. |
| `Makefile`, `install`, `uninstall`, `README.sdk` | Various | Reference only. |

## Why we keep it

This SDK is the only currently-published source that documents the
AMD-RAID on-disk struct layouts and the API the proprietary core
exposes. Without it, every layout would have to be reverse-engineered
from `rcraid.sys` from scratch, which would extend the project's
timeline considerably.

Using DWARF *layout* information from a third-party binary for
interoperability is generally protected under DMCA §1201(f); see the
repository-root `RE_METHODOLOGY.md` for the full legal record.

## What we do NOT do

- Compile this SDK
- Link `rcblob.x86_64` into anything we ship
- Copy AMD's GPL'd C sources into our driver
- Redistribute AMD's binaries to end users (we keep them in-repo only
  for the development team's convenience; downstream packages of
  `rcraid.ko` will not include this directory)

If you are AMD and have concerns about this material's presence in
the repository, please open an issue.
