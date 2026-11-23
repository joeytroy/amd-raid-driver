# After reboot — continuation instructions

State as of **2026-11-08**, `main` = `a2d9e2b` + this file. Read this top to
bottom before touching anything in the UEFI.

> **Status update 2026-11-08 (later): steps 1–2 are done.** The RAIDXpert2
> 3-disk RAID 5 exists and its metadata parses from the members (`0x1BF5`,
> `Devices=3`, `Capacity = 2 × UserDataSize`, `ChunkIndex=3` → 256 KiB).
> Do **not** re-create the array. The next action is the load/read run; on the
> current live-USB session use `scripts/raid5-live-test.sh` (or the manual
> bind/load steps in §4 for the installed OS). That run is blocked only on the
> one-time MOK enrollment for the live session's local signing key — the
> installed OS's `minerva` key is not present in this session. See issue #1
> for the recorded geometry and `docs/VALIDATION_RAID5.md` for the layout
> check that is still open (issue #6).

> **Correction 2026-11-09: that array no longer exists.** The 3-disk RAID 5
> has been removed, so nothing below can be re-run against it. The
> observations recorded here and in `docs/VALIDATION_RAID5.md` are the only
> record of it; re-running the read-only checks needs a freshly created
> firmware/RAIDXpert2 array. Keep this file's history, but do not treat the
> array as present.

**Why:** the firmware array can only be created in the RAIDXpert2 UEFI
utility. That is the only reason — on the installed OS the module signing key
(`CN=minerva Secure Boot Module Signature key`) is **already enrolled** in MOK
and its fingerprint matches `/var/lib/shim-signed/mok/MOK.der` there, so
signing `rcraid.ko` works without a second reboot **on that system**. The
live-USB session uses a separate local key; see the status note above.

**The rule for this run: the driver stays out of the initramfs.** Do not run
`install-dkms.sh`, `install-livecd.sh`, or `update-initramfs` before the array
exists and has been read-validated by hand. The packaged udev rule keys off
`subsystem_vendor`, which is `0x1987` on all four drives (OS included), and
`rcraid-bind` would unbind `nvme` from the OS controller before root is
mounted. See "Why not the initramfs" at the end.

---

## 1. Before you reboot

Record which physical drive the OS is on — you will need the serial inside the
UEFI utility:

```bash
lsblk -o NAME,SIZE,SERIAL,MOUNTPOINTS
findmnt -no SOURCE,FSTYPE /
```

Last seen: OS = `/dev/nvme2n1p2` (XFS `/`) on the controller at **16:00.0**,
with `nvme0n1`, `nvme1n1`, `nvme3n1` blank and available. Confirm, don't assume.

## 2. In the UEFI (RAIDXpert2)

**Del** at boot → *Advanced* → *RAIDXpert2 Configuration Utility* → *Array
Management* → *Create Array*.

- Select the **three blank Sabrent drives only**, identified by serial — not
  by position or model string.
- RAID level **RAID 5**, all three members, *Create Array*, then *Apply
  Changes* and confirm the array reads `Optimal`.
- **Do not include the OS drive.** Putting it in the array makes the machine
  unbootable, and the data on it is not recoverable from this session. Tick
  exactly these three serials and nothing else:

  | Slot | Serial | Size |
  |------|--------|------|
  | `03:00.0` | `7C550718046F00004936` | 3.6 T |
  | `11:00.0` | `912807140FF600124174` | 3.6 T |
  | `15:00.0` | `9128071610F600182290` | 3.6 T |

  The OS drive is `912807140FF600124197` at `16:00.0` — it is the same model,
  size and firmware as the others, so serial is the only reliable identifier.

- Before saving and exiting: **Boot** tab → confirm `ubuntu` is still first in
  boot order. Creating an array can add an `AMD RAID`-style entry, and if that
  entry is selected the firmware finds no bootable image on the array and the
  boot fails. The `ubuntu` entry lives on `nvme2n1p1` and stays valid as long
  as the array does not include that drive.

RAID 6 needs four members, so it is not possible with the current disk set.
A 3-disk RAID 5 gives you 2 disks of usable capacity and single-disk
redundancy.

## 3. First checks after boot (driver not loaded yet)

```bash
uname -r                              # expect 7.3.0-10-generic
lsblk -o NAME,SIZE,TYPE,MOUNTPOINTS   # no rcraid0 yet — that is correct
lspci -d 1022:b000
sudo dmesg | grep -i rcraid           # should be silent
```

Linux cannot see the array until `rcraid` is loaded; the volumes only appear
after the module parses the firmware metadata.

### 3a. MOK first — it costs a reboot, so settle it before step 4

An unsigned `rcraid.ko` is not loadable with Secure Boot enforcing, and
enrolment only completes inside the MOK manager on the **next** boot. So this
is the first thing to resolve after the reboot, not something to discover when
`insmod` fails. A BIOS reset or CMOS clear silently wipes MOK, so re-check
rather than assume:

```bash
mokutil --sb-state                            # "SecureBoot enabled"/"disabled"
mokutil --list-new | grep -iE "subject|minerva" # queued, not yet enrolled
mokutil --list-enrolled | grep -i minerva      # expect a SHA1 line + subject
```

Pick the branch that matches:

- **Queued** (`--list-new` has entries): reboot now, *Enroll MOK* → *Continue* →
  the one-time password, then reboot again and resume at §3. The pending entry
  was imported earlier; nothing needs to be regenerated.
- **Enrolled, and `/var/lib/shim-signed/mok/MOK.priv` matches it** (the installed
  OS, where the key is `CN=minerva Secure Boot Module Signature key`): no
  action — sign with those two paths in step 4.
- **Neither**: create a key and enrol it. Store it where it survives the reboot
  — a live session's `/tmp` or overlay does not, and an enrolled certificate
  whose private key is gone cannot sign anything, which strands the module for
  good:

  ```bash
  KEYDIR=/mnt/usb/mok          # any path on persistent storage
  mkdir -p "$KEYDIR" && cd "$KEYDIR"
  openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
      -subj "/CN=rcraid module signing $(date +%Y-%m-%d)/" \
      -keyout MOK.priv -outform DER -out MOK.der
  sudo mokutil --import MOK.der    # set a one-time password
  ```

  Reboot, *Enroll MOK* → *Continue* → password, reboot again. Then sign in
  step 4 with `"$KEYDIR"/MOK.priv` and `"$KEYDIR"/MOK.der`.

Check the result before moving on — `modinfo -F signer ./rcraid.ko` should name
the key, and `dmesg | grep -i "key was rejected"` must stay empty after load.

## 4. Build, sign, load

```bash
cd /home/chazz/amd-raid-driver
make -C /lib/modules/$(uname -r)/build M="$PWD" modules

sudo "/usr/src/linux-headers-$(uname -r)/scripts/sign-file" sha256 \
    /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der \
    rcraid.ko

Use the paths belonging to whichever key §3a ended up with — signing with a
cert that is not enrolled leaves the module unloadable with the same
`Key was rejected by service`. Verify before loading:

```bash
modinfo -F signer ./rcraid.ko      # must name the enrolled key
```

```bash
sudo insmod ./rcraid.ko
lsblk
sudo dmesg | tail -40
```

`insmod` alone will print `probe` messages for **nothing**: the PCI core never
steals a device already bound to `nvme`, so the module loads and claims 0
adapters. That is expected, not a failure. Because the udev rule and initramfs
hook are deliberately **not** installed (the rule keys off `subsystem_vendor`,
which is `0x1987` on all four drives, so it cannot tell the OS disk apart),
hand the three array members to `rcbottom` by hand — **never `16:00.0`, that is the OS drive**:

```bash
for d in 0000:03:00.0 0000:11:00.0 0000:15:00.0; do
    echo rcbottom | sudo tee /sys/bus/pci/devices/$d/driver_override
    echo "$d"    | sudo tee /sys/bus/pci/drivers/nvme/unbind
    echo "$d"    | sudo tee /sys/bus/pci/drivers_probe
done
lsblk
sudo dmesg | tail -40
```

Confirm the slots are the members before trusting them — serials are
`7C550718046F00004936` (03), `912807140FF600124174` (11),
`9128071610F600182290` (15); the OS drive is `912807140FF600124197` (16).

To undo (rebind to `nvme`, no reboot needed):

```bash
sudo rmmod rcraid
for d in 0000:03:00.0 0000:11:00.0 0000:15:00.0; do
    sudo sh -c "echo '' > /sys/bus/pci/devices/$d/driver_override"
    echo "$d" | sudo tee /sys/bus/pci/drivers_probe
done
```

Expect `rcraid0` to appear with the level the metadata declares. Inspect it:

```bash
cat /sys/block/rcraid0/rcraid/state
cat /sys/block/rcraid0/rcraid/members
cat /sys/block/rcraid0/size
```

## 5. What a 3-disk RAID 5 should and should not do

This tree **assembles and reads parity volumes but refuses writes** — that is
the current Phase 1 behaviour, not a regression.

```bash
# read must succeed (O_DIRECT so the page cache cannot mask a failure)
sudo dd if=/dev/rcraid0 of=/dev/null bs=1M count=64 iflag=direct status=progress

# write must be refused with EIO; nothing on the disks should change
sudo dd if=/dev/zero of=/dev/rcraid0 bs=1M count=1 conv=fsync
```

Record the outcome in `docs/STATUS.md` under a new dated entry: whether the
volume assembled, the level the driver reported, the read result, the exact
write-refusal message, and the member tuple from the LD record.

Do **not** `mkfs`, mount read-write, or otherwise treat it as a usable volume,
and do not expect degraded operation to be safe — parity rebuild is not
implemented (only RAID 1/RAID 10 resync exists), so a 3-disk RAID 5 missing a
member is read-only at best.

## 6. If the array does not assemble

Capture, in this order:

```bash
sudo dmesg | grep -iE "rcraid|nvme" > /tmp/reboot-dmesg.txt
sudo dd if=/dev/nvme0n1 bs=512 skip=20480 count=1 2>/dev/null | head -c 32 | xxd
sudo python3 scripts/qemu-test/mkmeta.py --help
```

The sector at LBA `0x5000` must carry magic `RAIDCore` at `+0x08` (the first
8 bytes are the checksum) — `mkmeta.py` writes exactly that layout to files or
real block devices. Compare the LD record's device tuple against the
X399 fixtures in `PlaidPiper/x399-raidcore-fixtures`; a new tuple shape is a
parser gap, not corruption. Write findings to `docs/STATUS.md` before fixing
anything.

## 7. Work queue once the hardware run is recorded

Order from `docs/IMPLEMENTATION.MD` ("Suggested next-PR order"):

1. **Metadata writer for RAID 10** — `scripts/qemu-test/mkmeta.py` lists
   `raid5`/`raid10` in `LEVELS` but `main()` exits with "metadata is untested"
   for both, so those choices dead-end. 3×2 RAID 10 adjacency is still
   unconfirmed (`docs/VALIDATION_RAID10.md`, step 1) — prefer 2×2 until checked.
2. **Verify `/dev/amd_ctl0`** — the bridge is now registered as a `misc`
   device and routed at the live volume. After the module loads, check the
   node exists, `GET_STATUS`/`GET_HARDWARE_INVENTORY` work as a normal user,
   and `FAIL_MEMBER` returns `-EPERM` without root. `START_RESYNC` is still
   RAID 1/10 only; RAID 5 correctly returns `-EOPNOTSUPP`.
3. **H-5** — trace where `ctrl_mode` is set relative to `spin_lock_init()` and
   IRQ registration in `rc_hw_init()`, then annotate `docs/AUDIT.MD` as fixed
   or open. It is the only High still marked "not re-checked".
4. **Support matrix** — if this boot is on a kernel newer than the
   `docs/README.md` claim, update the validated-through version.

Regression testing without hardware (no reboot needed, but `/tmp` was cleared):

```bash
./scripts/qemu-test/run-qemu-raid-test.sh --kver "$(uname -r)" \
    --level raid1 --fail-member --workdir /tmp/raid-check
```

Degraded mode is selected by flag, not by level name — `--fail-member` kills
a member at runtime, `--omit-member` boots with one detached (`allow_degraded=1`).
CI runs exactly those four: `raid0`, `raid1`, `raid1 --fail-member`,
`raid1 --omit-member`.

## 8. Guardrails

- **Never** write to `16:00.0` / `/dev/nvme2n1*` — that is the live OS disk.
- Baseline the machine again before any test: `rcraid` not loaded, no
  `/dev/amd_ctl0`, `/` still on XFS.
- `gh` is authenticated user-wide via the keyring token; if it expires,
  `gh auth login --web` and `gh auth setup-git` restores it.

## 9. Why not the initramfs (until the rule is fixed)

`packaging/udev/50-rcraid.rules.in` fires on **every** `1022:b000` device whose
`subsystem_vendor` matches `@SUBSYSTEM_VENDOR@` and calls
`packaging/sbin/rcraid-bind`, which sets `driver_override=rcbottom` and unbinds
`nvme`. The filter exists to protect the OS disk — but on this box all four
drives report `0x1987/0x5012`, so it matches the OS disk too.

Inside an initramfs that sequence is unrecoverable: `nvme` is unbound from
`16:00.0`, `rcbottom` probes it and resets the controller (`CC.EN=0`), and
because `nvme2n1` carries no `RAIDCore` metadata no volume is assembled and no
block device appears. `/dev/nvme2n1p2` never exists, so `pivot_root` fails.
Recovery is a live USB or a GRUB `modprobe.blacklist=rcraid` edit.

Binding by hand (step 4) is the same operation, scoped to three BDFs, run after
the OS is already up.

The fix that makes the initramfs path safe is now in `rcraid-bind` (issue #8):
the udev rule fires on the namespace rather than the PCI function, and the
helper reads the RAIDCore metadata at LBA `0x5000` — present on members,
absent on every other `1022:b000` disk, including this box's OS drive — before
touching `driver_override` or `nvme`. It also refuses a namespace that is
mounted. Re-check `mokutil`/signing first, but this removes the reason to keep
the initramfs hook uninstalled.
