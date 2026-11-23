#!/usr/bin/env python3
"""Diff rc_meta_build.h's output against mkmeta.py's, byte for byte.

src/test_metabuild_userspace.c writes the images the in-kernel encoder
builds; this script builds the same arrays with scripts/qemu-test/mkmeta.py
— the generator the QEMU rig assembles arrays from — and compares them.
A mismatch means the driver's writer and the rig disagree about the
metadata layout, which would make a created array unreadable by the
driver that wrote it.

Usage: check-metabuild.py <dir-with-c-output>
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Must match the constants in src/test_metabuild_userspace.c.
DISK_SECTORS = 0x200000
GEN_TS = 0x5243544553540A11
GEN_SEQ = 2
CHUNK_INDEX = 3
CHUNK_SECTORS = 512
ID_BASE = 0x5243544553540000
ARRAYS = (
    # (target filename tag, member count, FirstCount, SecondCount)
    ("raid0", 4, 4, 1),
    ("raid1", 2, 1, 2),
)


def load_mkmeta():
    spec = importlib.util.spec_from_file_location(
        "mkmeta", os.path.join(HERE, "mkmeta.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def expect(mk, tag, members, first, second):
    """(name, bytes) pairs mkmeta.py would write for this array."""
    ids = [ID_BASE + i for i in range(members)]
    user = (DISK_SECTORS - mk.USERDATA_START) // CHUNK_SECTORS \
        * CHUNK_SECTORS
    capacity = user if second == 2 and first == 1 else user * members
    level = "raid0" if first != 1 else "raid1"

    records = mk.build_records(level, ids, capacity, user, CHUNK_INDEX,
                               DISK_SECTORS)
    gen = mk.build_generation(GEN_TS, records)
    commit = mk.build_commit_block(mk.CONFIG_RING_LBA + 16, len(gen), GEN_TS)

    out = {}
    for i, did in enumerate(ids):
        out[f"md_{tag}_{i}.bin"] = mk.build_metadata_block(did)
    out[f"gen_{tag}.bin"] = gen
    out[f"commit_{tag}.bin"] = commit
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.splitlines()[0] + "\nusage: check-metabuild.py <dir>")
    outdir = sys.argv[1]
    mk = load_mkmeta()
    failures = 0

    for tag, members, first, second in ARRAYS:
        expected = expect(mk, tag, members, first, second)
        for name, want in expected.items():
            path = os.path.join(outdir, name)
            try:
                with open(path, "rb") as f:
                    got = f.read()
            except OSError as exc:
                print(f"FAIL {name}: {exc}")
                failures += 1
                continue
            if got == want:
                print(f"ok   {name} ({len(want)} B)")
                continue
            failures += 1
            print(f"FAIL {name}: {len(got)} B vs mkmeta {len(want)} B")
            for off in range(min(len(got), len(want))):
                if got[off] != want[off]:
                    print(f"     first difference at +0x{off:x}: "
                          f"c=0x{got[off]:02x} mkmeta=0x{want[off]:02x}")
                    break

    if failures:
        sys.exit(f"check-metabuild: {failures} mismatch(es)")
    print("check-metabuild: encoder matches mkmeta.py")


if __name__ == "__main__":
    main()
