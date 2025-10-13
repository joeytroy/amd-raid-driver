#!/bin/bash

echo "=== AMD RAID Protocol Clues Extraction ==="
echo

echo "=== Command Opcodes Found ==="
echo "From rcraid.sys strings:"
strings windows/rcraid/rcraid.sys | grep -i -E "(command|opcode|0x[0-9a-f]{2,4})" | grep -v "Unknown command" | head -20
echo

echo "=== Register/MMIO References ==="
echo "Looking for MMIO patterns:"
strings windows/rcraid/rcraid.sys | grep -i -E "(0x[0-9a-f]{8}|register|mmio|bar)" | head -10
echo

echo "=== Error Codes ==="
echo "Error handling patterns:"
strings windows/rcraid/rcraid.sys | grep -i -E "(error|fail|status|0x[0-9a-f]{2,4})" | head -15
echo

echo "=== Configuration Commands ==="
echo "Config-related commands:"
strings windows/rcraid/rcraid.sys | grep -i -E "(config|write.*command|read.*command)" | head -10
echo

echo "=== SCSI/StorPort References ==="
echo "SCSI layer integration:"
strings windows/rcraid/rcraid.sys | grep -i -E "(scsi|storport|srb|sense)" | head -10
echo

echo "=== Power Management ==="
echo "Power management features:"
strings windows/rcraid/rcraid.sys | grep -i -E "(power|hipm|dipm|idle)" | head -10
echo

echo "=== Analysis Complete ==="
