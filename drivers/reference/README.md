# drivers/reference/

This directory holds **third-party material kept as reference only**.
None of the files here are compiled, linked, or otherwise
incorporated into the produced `rcraid.ko` kernel module.

## Why it's separate

Keeping reference material in a dedicated tree (rather than mixed
into the working sources) makes it unambiguous which files are part
of this driver's GPL-2.0 codebase and which are external artifacts
under their own licenses.

## What's in here

### `amd-sdk-9.3.0/`

The AMD-shipped Linux RAID driver SDK, version 9.3.0-00283. Two
copies of essentially the same content:

- `raid_linux_driver_930_00283/` — RHEL DKMS bundle
- `raidxpert2_linux_930_00283/` — broader bundle including the
  `rcadm` userspace tool and PDF documentation

Both ship `driver_sdk/src/` with:

- AMD's GPL-licensed kernel-glue C sources (e.g. `rc_init.c`,
  `rc_config.c`, `rc_msg.c`). **We do not copy from these** — they
  are read only to understand AMD's API conventions.
- `rcblob.x86_64` — AMD's proprietary RAID core, distributed as an
  ELF object with DWARF debug info. The SDK ships under AMD's own
  `LICENSE_SDK` (see `driver_sdk/LICENSE_SDK`); we use the DWARF
  *layout* information for interoperability purposes only and never
  link against the binary or copy its source.

See the repository-root `RE_METHODOLOGY.md` for the full process and
legal framing.

### Other AMD reference files

The AMD Windows binaries (`.sys` / `.inf` / `.cat`) live in
`drivers/windows/trx50/` rather than here because they ARE the
primary input to our clean-room reverse engineering, not just
secondary reference material.
