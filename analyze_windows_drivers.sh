#!/bin/bash

echo "=== AMD RAID Driver Analysis ==="
echo "Analyzing Windows driver files for firmware protocol clues..."
echo

echo "=== Device IDs from INF files ==="
echo "rcbottom.inf supports:"
grep -E "VEN_1022&DEV_[0-9A-F]+" windows/rcbottom/rcbottom.inf
echo

echo "=== MSI Configuration ==="
echo "MSI settings:"
grep -i "msi\|interrupt" windows/rcbottom/rcbottom.inf
echo

echo "=== Power Management Settings ==="
echo "HIPM/DIPM configuration:"
grep -i "hipm\|dipm\|hmb" windows/rcbottom/rcbottom.inf
echo

echo "=== Driver Dependencies ==="
echo "rcraid depends on:"
grep -i "dependencies" windows/rcraid/rcraid.inf
echo

echo "=== SCSI Configuration ==="
echo "SCSI settings:"
grep -i "scsi\|storport" windows/rcraid/rcraid.inf
echo

echo "=== Binary Analysis (if available) ==="
if command -v strings >/dev/null 2>&1; then
    echo "Strings from rcbottom.sys:"
    strings windows/rcbottom/rcbottom.sys | grep -i -E "(command|opcode|register|0x[0-9a-f]{2,4})" | head -10
    echo
    
    echo "Strings from rcraid.sys:"
    strings windows/rcraid/rcraid.sys | grep -i -E "(command|opcode|register|0x[0-9a-f]{2,4})" | head -10
    echo
else
    echo "strings command not available - install binutils"
fi

echo "=== Analysis Complete ==="
