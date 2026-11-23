# FEATURE PROPOSAL: Fixing Mirror Performance with Asynchronous Workers + RAID 5/6 Math Specs

## 1. The RAID 1 Performance Fix
The driver currently processes mirrored (RAID 1) writes synchronously, which can freeze up execution queues while waiting for physical NVMe targets to respond.

To fix this, I have written a cleanroom background worker engine (`patch_async_worker.c`). It sets up a background helper thread (`kamd_async_worker`) that immediately handles write confirmations asynchronously, freeing up the primary Linux block layer queues to keep pushing files without pausing.

I have verified that this code hooks cleanly into the driver and compiles with 100% success on a modern kernel (`7.2.4-4-liquorix-amd64`). Because I do not have physical AMD hardware RAID arrays at my current workstation to run live benchmarks, I am sharing this compiled code blueprint so developers with active hardware can test and run performance benchmarks.

---

## 2. The Missing Math Specs for RAID 5/6
To help the community eventually move the project past basic RAID 0/1 layouts, here are the exact mathematical parameters and logic tracks used by the official proprietary driver to calculate parity stripes:

### RAID 5 Parity (XOR Math)
* **Logic:** Pre-calculated checksums using an Exclusive OR (`XOR`) loop across data blocks:
  $$\text{Parity Block} = D_1 \oplus D_2 \oplus D_3 \dots$$

### RAID 6 Parity (EVENODD diagonal, integer modulus 17)
* **P Vector Checksum:** Calculated as a standard multi-disk binary XOR sweep across a row.
* **Unit:** 512 bytes per member, split into sixteen 32-byte words. There are seventeen diagonal positions; one is skipped in each column and carried separately in an $S$ correction term.
* **S term:** $S = \bigoplus_{d \ge 1} D_d[16 - d]$ — each member contributes a single word, walking back one word per member from the last.
* **Q Vector Checksum:** Each Q word starts from $S$, then XORs one word from each data member at a modular diagonal position, skipping the member whose position lands on the skipped diagonal:
  $$\mathbf{Q}[c] = S \oplus \bigoplus_{d} D_d[(c - d) \bmod 17], \qquad d \text{ skipped when } (c - d) \bmod 17 = 16$$
* **Arithmetic:** XOR plus an integer modulus by 17. No Galois field, no multiplication, no exp/log tables — this is the tell when reading the binary, since a $GF(2^8)$ Q cannot avoid a multiply.
* **Capacity:** the $S$ walk caps a RAID 6 row at 16 data members.

> **Corrected 2026-11-08.** This section originally described Q as $GF(2^8)$ Reed-Solomon over `0x11D`. That is not what AMD's controller computes; Q built that way is silently incompatible with firmware and only shows up during recovery on the vendor stack. The trace is in [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md) item 8, and the code is `src/raid6_evenodd.h` (issue #10).

---

## Next Steps
I have the working patch code compiled successfully inside my local workspace. Let me know if you would like me to submit a formal Pull Request (PR) to drop these files directly into the repository!
