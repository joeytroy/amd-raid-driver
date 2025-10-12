#!/bin/bash

echo "=== TRX50 RAID Detection Test Script ==="
echo

echo "1. Checking if rcraid driver is loaded:"
lsmod | grep rcraid
echo

echo "2. Checking driver messages:"
sudo dmesg | grep -i "rcraid\|trx50" | tail -20
echo

echo "3. Checking PCI devices:"
lspci | grep -i "raid\|nvme"
echo

echo "4. Checking SCSI hosts:"
ls -la /sys/class/scsi_host/
echo

echo "5. Checking SCSI devices:"
ls -la /sys/class/scsi_device/
echo

echo "6. Current block devices:"
lsblk
echo

echo "7. Checking for RAID management tools:"
which storcli 2>/dev/null || echo "storcli not found"
which megacli 2>/dev/null || echo "megacli not found"
which amd-raid-config 2>/dev/null || echo "amd-raid-config not found"
echo

echo "8. Checking kernel parameters:"
cat /proc/cmdline
echo

echo "9. Checking loaded modules:"
lsmod | grep -E "(ahci|nvme|rcraid)"
echo

echo "10. Forcing SCSI rescan:"
echo "- - -" | sudo tee /sys/class/scsi_host/host*/scan
echo

echo "11. Checking block devices after rescan:"
sleep 2
lsblk
echo

echo "12. Checking for new SCSI devices:"
ls -la /sys/class/scsi_device/
echo

echo "=== Test Complete ==="
echo "If your 3.9TB RAID0 array is still not showing, the issue might be:"
echo "1. RAID arrays not properly configured in BIOS"
echo "2. Driver not communicating with RAID controller properly"
echo "3. Missing RAID management interface"
echo "4. Need to manually trigger RAID array activation"
