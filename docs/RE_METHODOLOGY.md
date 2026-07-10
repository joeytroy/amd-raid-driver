# Reverse-Engineering Methodology

This document records the clean-room methodology used to develop the
`rcraid` Linux kernel module. Its purpose is to establish, on the
record, that the code in this repository is independently authored
and derived only from interoperability-protected reverse engineering
of lawfully obtained binaries — not from any AMD source code.

## Legal basis

The reverse-engineering work in this project is performed under the
following protections:

- **17 U.S.C. § 1201(f) — DMCA reverse-engineering exception for
  interoperability.** This section explicitly permits circumventing
  technological protection measures and analyzing the workings of a
  lawfully obtained computer program "for the sole purpose of
  identifying and analyzing those elements of the program that are
  necessary to achieve interoperability of an independently created
  computer program with other programs."
- **17 U.S.C. § 117(a)(1) — adaptation as essential step.** The
  owner of a copy of a computer program is permitted to make
  adaptations as an essential step in the utilization of the program
  in conjunction with a machine.
- **Sega v. Accolade, 977 F.2d 1510 (9th Cir. 1992).** Disassembly
  for the purpose of studying functional requirements for
  compatibility is fair use.
- **Sony v. Connectix, 203 F.3d 596 (9th Cir. 2000).** Intermediate
  copying during reverse engineering for interoperability is fair use.
- **Atari Games Corp. v. Nintendo of Am. Inc., 975 F.2d 832 (Fed.
  Cir. 1992).** Clean-room reverse engineering of an undocumented
  hardware-interaction protocol is protected.
- **EU Software Directive 2009/24/EC, Article 6** — equivalent
  reverse-engineering-for-interoperability protection in the EU.

This is not legal advice. Maintainers and contributors who are
uncertain about the legal posture in their own jurisdiction should
consult counsel — the Software Freedom Law Center
(<https://softwarefreedom.org/>) provides free consultations to
open-source projects in exactly this situation.

## Inputs to the reverse engineering process

The following lawfully obtained binaries are used as study material:

| File | Source | Purpose |
|------|--------|---------|
| `drivers/windows/trx50/9.3.{2,3}-*/rcbottom/rcbottom.sys` | AMD's free public driver-download page (<https://www.amd.com/en/support>) | Per-PCI-device PnP driver. Studied for PCI/NVMe register programming, queue init, completion-handling logic. |
| `drivers/windows/trx50/9.3.2-*/rcraid/rcraid.sys` | Same | RAID virtualization layer. Studied for on-disk metadata layout, member-config handling, RAID-level dispatch. |
| `drivers/windows/trx50/9.3.{2,3}-*/rcconfig/rccfg.sys` | Same | User-space IOCTL surface for the `rcadm` tool. Reference only — we do not currently implement this. |
| `drivers/reference/amd-sdk-9.3.0/.../rcblob.x86_64` | AMD's free public Linux driver SDK | Proprietary RAID core, distributed as an ELF object with DWARF debug info. Used as **secondary cross-reference** only — never linked, never copied. Layout extracted via `pahole`/`objdump`. |
| `drivers/reference/amd-sdk-9.3.0/.../*.c`, `*.h` | Same SDK | AMD's GPL'd open-source kernel-glue layer. Read for **API understanding only**. None of its code is included in this driver. |

All Windows binaries (.sys / .inf / .cat) were obtained from AMD's
publicly accessible driver download page. No Windows installation
media or system image was used to obtain the files; the distributed
driver bundle is the sole source.

The AMD distribution page presents end-user license terms in a
**browse-wrap** form (notice on the download page rather than a
required "I Agree" checkbox).  Those terms purport to prohibit
reverse engineering of the binaries.  We acknowledge this and
proceed under the following position, which is asserted in good
faith but not adjudicated:

1. The statutory interoperability exception in 17 U.S.C. § 1201(f)
   is a federal carve-out that, on the better view, cannot be
   waived by a contractual term in a browse-wrap notice.  *Bowers v.
   Baystate Technologies*, 320 F.3d 1317 (Fed. Cir. 2003) is cited
   for the contrary position; *Specht v. Netscape Communications*,
   306 F.3d 17 (2d Cir. 2002) and similar browse-wrap cases support
   the view that mere notice without affirmative acceptance is
   inadequate to form a binding contract.
2. The interoperability work in this repository is the minimum
   necessary to allow an independently authored Linux driver to
   interoperate with AMD-RAID hardware that lawful purchasers of
   that hardware already own; no AMD code is copied into this
   project, and the produced kernel module does not link against
   any AMD-distributed binary.
3. The primary IP holder of the Software per the EULA's own
   Proprietary Rights section is Seagate Technology PLC (AMD
   distributes under license from Seagate); the analysis above
   applies equally to either party.

Maintainers and contributors who are concerned about the legal
posture in their own jurisdiction should consult counsel before
publishing, distributing, or contributing.

The Linux SDK was likewise obtained from AMD's public driver page.
It ships with its own license file (`LICENSE_SDK`) which restricts
use of the SDK. We do not include any of the SDK's source code in
this driver — only struct *layouts* (offsets and sizes) derived from
its DWARF debug information, where those layouts describe an
on-disk or on-wire data format that is necessary for interoperability
with arrays previously configured by AMD's Windows driver.

## What the methodology does NOT do

- **No code is copied from AMD's binaries into this driver.** Every
  line of source under `rc_*.c` and `rc_linux.h` is original work,
  authored from scratch to implement behaviour observed through
  reverse engineering.
- **No code is copied from AMD's open-source Linux SDK.** The SDK's
  `rc_*.c` glue layer is for reference only; our driver does not
  share file names, function names, control flow, or comments with
  it. (Some identifier names match — e.g., `RC_MetaData`, `Devices`,
  `ChunkSize`, `ConfigCommitOffset` — because they describe
  interoperability-relevant fields whose names are functional
  requirements of the protocol. Equivalent treatment of API names is
  long-established under *Lotus Dev. Corp. v. Borland Int'l*, 49
  F.3d 807, and *Google LLC v. Oracle America, Inc.*, 593 U.S. ___
  (2021).)
- **No proprietary binary is linked into the resulting `rcraid.ko`.**
  The kernel module is self-contained GPL-2.0 code that talks to the
  NVMe controllers directly via standard NVMe-spec commands.
- **No reverse engineering is done to circumvent licensing or DRM
  restrictions on the data the array contains.** The work is solely
  about the protocol the driver speaks to the controller and the
  on-disk format of metadata that the driver itself writes.

## What we DID do (process)

1. **Acquired Windows binaries** from AMD's free public driver-download
   page. No EULA was accepted; the driver package is a self-extracting
   archive of `.sys`, `.inf`, and `.cat` files plus a small installer.
2. **Disassembled the .sys files** with `ghidra` (headless, scripted
   under `scripts/ghidra/`) to identify register access patterns,
   command-submission formats, and on-disk structure layouts.
3. **Cross-referenced via the open Linux SDK's `rcblob.x86_64`** —
   an ELF with DWARF debug info, dumped via `pahole` and `objdump`,
   to confirm struct offsets and enum values. Used only as a sanity
   check against the disassembly; no SDK source is in this driver.
4. **Implemented `rcraid.ko` from scratch** in C against the public
   Linux kernel headers, using only the protocol/layout information
   gathered above. Each non-trivial design choice is recorded in
   `docs/REVERSE_ENGINEERING.md`, `docs/STATUS.md`, or
   `docs/REVERSE_ENGINEERING.md`.
5. **Validated empirically** on real AMD-RAID hardware (TRX50 + two
   Crucial T700 NVMe drives configured as RAID0 in the BIOS) by
   running the driver and comparing observed behaviour to what
   `rcraid.sys` does on the same hardware.

## Per-source-file pedigree

Every C / header file in this repository carries an SPDX license
identifier (`GPL-2.0-only`) and a copyright header crediting Joey Troy
and contributors. The headers also state that the file is original
work derived only from interoperability-protected reverse engineering.

The directory `drivers/reference/amd-sdk-9.3.0/` contained AMD-shipped
source (under their `LICENSE_SDK`) and the closed `rcblob.x86_64`.
These files were reference material only. They were never compiled
into, linked with, or otherwise incorporated into the produced kernel
module, and have since been removed from the tree entirely — they
remain in git history (`git checkout 69738ee -- drivers/`) and on
AMD's public download pages.

## If you are AMD

Hello. This project is an interoperability driver written from scratch
under DMCA §1201(f) so that Linux users with hardware you sold can
access their data. We do not redistribute your binaries beyond what
is necessary to document the protocol (and we link to your download
page rather than mirroring). We would be very happy to coordinate
with you — please open an issue on the repository or contact the
maintainer if you would like to discuss documentation, hardware
references, or any concerns.
