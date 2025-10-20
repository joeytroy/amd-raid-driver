# AMD RAID Driver - Alpha Testing Checklist

## Implementation Status: Complete

All 10 workflow steps from the GPT5 plan have been implemented:

### ✅ Completed Steps

1. **Step 1-2: PCI Probe & BAR Mapping**
   - [x] Strict PCI ID checking (1022:43bd)
   - [x] BAR5 MMIO mapping (1024 bytes)
   - [x] 64-bit DMA with 32-bit fallback
   - [x] MSI/MSI-X with legacy fallback
   - [x] Device extension structure (rc_dev_context)

2. **Step 3-4: Queue Management**
   - [x] Queue descriptor allocation (0x78-byte, FUN_14000d66c)
   - [x] DMA control blocks (0x400-byte, FUN_14000655a)
   - [x] Completion register programming (FUN_1400023BB)
   - [x] Doorbell activation with timing (5µs + 25µs stalls)
   - [x] Descriptor table management (devExt+0x1C2A0)

3. **Step 5: blk-mq Integration**
   - [x] Async request submission
   - [x] Request tracking (256-slot hash table)
   - [x] DMA buffer management
   - [x] Hardware → blk_status_t mapping
   - [x] Queue depth matching Windows (32 entries)

4. **Step 6-7: Interrupt Handling**
   - [x] ISR sequence (FUN_14000d2b8)
   - [x] Queue handle iteration (6B8/6C0)
   - [x] Completion pump (+0x680)
   - [x] Scratch pointer management (670/698)
   - [x] Pending count tracking (678)

5. **Step 8: Configuration & Monitoring**
   - [x] Sysfs interface (adapter info, queue stats, doorbell state)
   - [x] Debugfs interface (queue dumps, register inspection)
   - [x] Per-adapter monitoring

6. **Step 9-10: Testing & Documentation**
   - [x] Automated test script (test_driver.sh)
   - [x] Updated README.md
   - [x] Updated INSTALL.md
   - [x] Testing checklist (this document)

---

## Hardware Testing Plan

### Phase 1: Driver Load & Basic Validation

**Expected Environment:**
- TRX50 motherboard with AMD RAID controller (1022:43bd)
- RAID arrays configured in BIOS
- Kubuntu 24.04 LTS Live USB
- AHCI drivers blacklisted in GRUB

**Tests:**

1. **Build Driver**
   ```bash
   cd /path/to/amd-raid-driver
   ./build.sh
   ```
   - [ ] Build completes without errors
   - [ ] rcraid.ko module created

2. **Run Automated Test**
   ```bash
   sudo ./test_driver.sh
   ```
   - [ ] Driver loads successfully
   - [ ] AMD RAID device detected (1022:43bd)
   - [ ] Sysfs entries created
   - [ ] No kernel panics or oopses

3. **Check Kernel Messages**
   ```bash
   dmesg | grep -i "rcraid\|rcbottom\|rc_"
   ```
   Expected messages:
   - [ ] "rc_bottom: probe VID=0x1022 DID=0x43bd"
   - [ ] "rc_hw_init: initializing hardware"
   - [ ] "rc_queue: initializing queue subsystem"
   - [ ] "rc_queue: activating doorbells"
   - [ ] "rc_bottom: adapter 0 ready"
   - [ ] NO errors about failed mappings or DMA allocation

### Phase 2: Hardware Communication Validation

4. **Check Sysfs Stats**
   ```bash
   cat /sys/bus/pci/drivers/rcbottom/*/rcraid/adapter_info
   ```
   - [ ] Correct PCI IDs displayed
   - [ ] IRQ mode shown (MSI/MSI-X/Legacy)
   - [ ] IRQ vector assigned

5. **Check Queue Status**
   ```bash
   cat /sys/bus/pci/drivers/rcbottom/*/rcraid/queue_stats
   ```
   - [ ] Command queue allocated (non-zero DMA address)
   - [ ] Completion queue allocated
   - [ ] Queue sizes = 32
   - [ ] IRQ count incrementing (watch for a minute)

6. **Check BAR Mapping**
   ```bash
   cat /sys/bus/pci/drivers/rcbottom/*/rcraid/bar_mapping
   ```
   - [ ] BAR5 mapped to 0x81a80000 (physical)
   - [ ] Length = 1024 bytes (0x400)
   - [ ] Virtual address assigned

7. **Check Doorbell State**
   ```bash
   cat /sys/bus/pci/drivers/rcbottom/*/rcraid/doorbell_state
   ```
   - [ ] Adapter Active: yes
   - [ ] Fast Path Enabled: yes

### Phase 3: Debugfs Inspection

8. **View Command Queue**
   ```bash
   cat /sys/kernel/debug/rcraid/adapter0/cmd_queue
   ```
   - [ ] Queue size = 32
   - [ ] DMA address valid
   - [ ] Head/tail positions shown

9. **View Completion Queue**
   ```bash
   cat /sys/kernel/debug/rcraid/adapter0/comp_queue
   ```
   - [ ] Queue size = 32
   - [ ] DMA address valid
   - [ ] Initially empty (no completions)

10. **View IRQ State**
    ```bash
    cat /sys/kernel/debug/rcraid/adapter0/irq_state
    ```
    - [ ] Primary queue handle set
    - [ ] IRQ count > 0 (after some time)
    - [ ] Queue table populated

11. **View Hardware Registers**
    ```bash
    cat /sys/kernel/debug/rcraid/adapter0/registers
    ```
    - [ ] MMIO base matches BAR5
    - [ ] Register values readable (not all 0xFF or 0x00)
    - [ ] Hex dump shows controller data

### Phase 4: Block Device Detection

12. **Check for Block Devices**
    ```bash
    ls -l /dev/rcraid*
    lsblk
    ```
    - [ ] /dev/rcraid0 (or similar) exists
    - [ ] Device appears in lsblk
    - [ ] Size matches configured RAID array

13. **Test Basic Read**
    ```bash
    sudo dd if=/dev/rcraid0 of=/dev/null bs=4096 count=1 iflag=direct
    ```
    - [ ] Read completes successfully
    - [ ] No I/O errors in dmesg
    - [ ] Completion count increments in queue_stats

14. **Monitor Live I/O**
    ```bash
    # Terminal 1: watch queues
    watch -n1 'cat /sys/bus/pci/drivers/rcbottom/*/rcraid/queue_stats'
    
    # Terminal 2: read test
    sudo dd if=/dev/rcraid0 of=/dev/null bs=4096 count=100 iflag=direct
    ```
    - [ ] Command sequence counter increments
    - [ ] IRQ count increments
    - [ ] No hung I/O (dd completes)

### Phase 5: Stress Testing

15. **Multiple Sequential Reads**
    ```bash
    for i in {1..10}; do
        sudo dd if=/dev/rcraid0 of=/dev/null bs=1M count=10 iflag=direct
        echo "Pass $i complete"
    done
    ```
    - [ ] All passes complete
    - [ ] No errors in dmesg
    - [ ] Pending requests count stays reasonable

16. **Check Pending Requests**
    ```bash
    cat /sys/bus/pci/drivers/rcbottom/*/rcraid/pending_requests
    cat /sys/kernel/debug/rcraid/adapter0/pending_requests
    ```
    - [ ] Pending count returns to 0 after I/O stops
    - [ ] No "stuck" requests

### Phase 6: Error Scenarios

17. **Unload/Reload Test**
    ```bash
    sudo rmmod rcraid
    sleep 2
    sudo insmod rcraid.ko
    ```
    - [ ] Unload succeeds
    - [ ] Reload succeeds
    - [ ] Device re-appears

18. **Check for Memory Leaks**
    ```bash
    # Before unload
    grep rcraid /proc/slabinfo
    
    # Unload
    sudo rmmod rcraid
    
    # After unload
    grep rcraid /proc/slabinfo
    ```
    - [ ] No leftover slab allocations after unload

---

## Known Limitations (Alpha)

- **Metadata Discovery**: Basic structure, needs real firmware protocol
- **RAID Operations**: No rebuild/resync support yet
- **Multi-Queue**: Single queue only (needs devExt+0x16068)
- **Error Recovery**: Basic timeout/retry logic missing
- **Performance**: No request batching or coalescing yet

---

## Debug Data Collection

If issues occur, collect:

1. **Full dmesg log**
   ```bash
   dmesg > dmesg.log
   ```

2. **All sysfs files**
   ```bash
   mkdir sysfs_dump
   cp /sys/bus/pci/drivers/rcbottom/*/rcraid/* sysfs_dump/
   ```

3. **All debugfs files**
   ```bash
   mkdir debugfs_dump
   cp /sys/kernel/debug/rcraid/adapter0/* debugfs_dump/
   ```

4. **lspci verbose**
   ```bash
   lspci -vvv -d 1022:43bd > lspci.log
   ```

5. **Register dump at failure**
   ```bash
   cat /sys/kernel/debug/rcraid/adapter0/registers > registers_at_failure.log
   ```

---

## Success Criteria

**Minimum (Phase 1-2):**
- Driver loads without errors
- Hardware detected and mapped
- Queues allocated and doorbells activated
- No kernel crashes

**Good (Phase 3-4):**
- All sysfs/debugfs data readable
- Block device appears
- Basic read operations work
- IRQ handling functional

**Excellent (Phase 5-6):**
- Sustained I/O with no errors
- Clean unload/reload cycle
- No memory leaks
- Proper completion tracking

---

## Next Steps After Testing

Based on test results:

1. **If Phase 1-2 fails**: Hardware register offsets may need adjustment
2. **If Phase 3-4 fails**: Firmware protocol/metadata discovery needs work
3. **If Phase 5 fails**: Timeout/error recovery needed
4. **If all pass**: Ready for firmware protocol implementation (OSIC)

Report all findings with the debug data collection above!

