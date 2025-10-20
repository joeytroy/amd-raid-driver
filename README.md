# AMD RAID Driver for Linux

> ⚠️ **WARNING: ALPHA DRIVER - TESTING PHASE** ⚠️
> 
> This is an **alpha driver** implementing real hardware communication:
> - ✅ Real PCI/BAR mapping and hardware register access
> - ✅ Hardware command/completion queues with DMA
> - ✅ MSI interrupt handling and async I/O completion
> - ✅ Queue management following Windows driver flow
> - ⚠️ **Early testing phase** - may have undiscovered bugs
> - ❌ **NOT for production** - testing and validation only
> 
> **Current Status:** Hardware communication implemented, alpha testing
> **Next Phase:** Extended testing, performance tuning, bug fixes

A modern Linux kernel driver for AMD RAID controllers, built from scratch based on Windows driver specifications. This driver is designed to provide full support for AMD RAID arrays on TRX50 and other AMD platforms.

## Current Implementation Status

### ✅ **Implemented (Alpha)**
- **Hardware Layer (rcbottom)**:
  - ✅ PCI device probe with strict ID checking (1022:43bd)
  - ✅ BAR mapping (all 6 BARs, focus on BAR5 for MMIO)
  - ✅ MSI/MSI-X interrupt allocation with legacy fallback
  - ✅ DMA mask setup (64-bit with 32-bit fallback)
  - ✅ Power management defaults (HIPM/DIPM from rcbottom.inf)

- **Queue Management (rc_queue)**:
  - ✅ Queue descriptor allocation (0x78-byte structures, FUN_14000d66c)
  - ✅ DMA control blocks (0x400-byte per queue, FUN_14000655a)
  - ✅ Completion register programming (FUN_1400023BB)
  - ✅ Doorbell activation with proper timing (5µs + 25µs stalls)
  - ✅ Descriptor table management (devExt+0x1C2A0)

- **Hardware Communication (rc_hw)**:
  - ✅ Command/completion queue setup (32 entries each)
  - ✅ DMA pool allocation for data transfers
  - ✅ Hardware register programming (queue addresses, sizes)
  - ✅ Command submission via doorbells
  - ✅ MSI interrupt handler (vector 244 on TRX50)
  - ✅ ISR sequence matching FUN_14000d2b8 (drain 6B8/6C0, clear scratch)
  - ✅ Completion pump with request tracking (256-slot hash table)

- **Block I/O (rc_blk)**:
  - ✅ blk-mq integration with async submission
  - ✅ Request tracking for ISR completion
  - ✅ DMA buffer allocation/management
  - ✅ Hardware status → blk_status_t mapping
  - ✅ Queue depth matching Windows (32 entries)

- **Monitoring (rc_sysfs)**:
  - ✅ Adapter info (PCI ID, IRQ mode, instance)
  - ✅ Queue statistics (head/tail, DMA addresses, counters)
  - ✅ Doorbell state (active, fast path, variants)
  - ✅ BAR mapping info (physical, virtual, sizes)
  - ✅ Pending request counts

- **Testing**:
  - ✅ Automated test script (test_driver.sh)
  - ✅ Sysfs monitoring
  - ✅ Basic I/O validation

### ⚠️ **Partial / In Progress**
- **Metadata Discovery**: Basic structure, needs real firmware protocol
- **RAID Array Detection**: Simulated data, needs firmware queries
- **Configuration Layer**: Basic interface, needs WMI equivalent
- **Error Handling**: Basic paths, needs comprehensive coverage

### ❌ **Not Yet Implemented**
- **Advanced Features**:
  - RAID rebuild/resync
  - SMART monitoring
  - Array creation/deletion from userspace
  - Hot-plug support
- **Performance Optimization**:
  - Multi-queue support (currently single queue)
  - Request batching/coalescing
  - NUMA awareness
- **Firmware Protocol**:
  - OSIC data structures
  - Firmware configuration commands
  - Metadata format parsing

## Development and Testing

This driver is currently in **alpha testing phase**:

- **Hardware Validation**: Testing real TRX50 RAID controller communication
- **Queue Management**: Verifying command submission and completion paths
- **Interrupt Handling**: Validating MSI and completion processing
- **I/O Operations**: Testing async read/write with DMA

### Quick Test

Run the automated test script after building:

```bash
sudo ./test_driver.sh
```

The script will:
1. Load the driver module
2. Check for AMD RAID hardware
3. Display sysfs statistics
4. Test basic I/O (if arrays configured)
5. Show queue status and adapter info

### Monitor Driver Activity

```bash
# Watch queue statistics in real-time
watch -n1 'cat /sys/bus/pci/drivers/rcbottom/*/rcraid/queue_stats'

# Check adapter info
cat /sys/bus/pci/drivers/rcbottom/*/rcraid/adapter_info

# Monitor doorbell state
cat /sys/bus/pci/drivers/rcbottom/*/rcraid/doorbell_state

# View pending requests
cat /sys/bus/pci/drivers/rcbottom/*/rcraid/pending_requests
```

### Testing Process
See [TESTING.md](TESTING.md) for comprehensive testing procedures and hardware validation steps.

### Next Development Steps
1. **Alpha Testing**: Validate on real TRX50 hardware with configured RAID arrays
2. **Firmware Protocol**: Decode and implement real OSIC metadata structures
3. **Multi-Queue**: Add support for multiple hardware queues (devExt+0x16068)
4. **Performance**: Request batching, completion coalescing, NUMA optimization
5. **Error Recovery**: Timeout handling, retry logic, error reporting
6. **Advanced RAID**: Rebuild, resync, array management operations

## System Requirements

### Tested Configuration
- **OS**: Kubuntu 24.04 LTS
- **Kernel**: 6.14.0-27-generic
- **Motherboard**: ASUS Pro WS TRX50-SAGE WIFI
- **Memory**: 16GB+ RAM (32GB recommended)
- **Storage**: Up to 14 devices (SATA + NVMe)
- **Max NVMe**: 10 devices
- **Max Controllers**: 11

### Supported Hardware

#### PCI Device IDs
- `0x1022:0x7905` - AMD Bristol RAID mode
- `0x1022:0x43BD` - AMD Promontory SATA controller
- `0x1022:0x7916` - AMD Summit RAID mode
- `0x1022:0x7917` - AMD X570S chipset RAID mode
- `0x1022:0xB000` - AMD NVMe RAID Bottom Device

#### Supported Platforms
- AMD TRX50 Threadripper PRO
- AMD WRX90 Threadripper PRO
- AMD X570S chipset
- AMD B550A chipset
- Other AMD RAID-enabled platforms

#### Build Dependencies
- `build-essential` - GCC, make, and build tools
- `linux-headers-$(uname -r)` - Kernel headers
- `linux-source-$(uname -r)` - Kernel source code (required for module building)
- `flex` - Lexical analyzer
- `bison` - Parser generator
- `libssl-dev` - SSL development libraries
- `libelf-dev` - ELF library development files
- `dwarves` - DWARF manipulation tools

## Installation

**See [INSTALL.md](INSTALL.md) for detailed installation instructions.**

## Architecture

The driver follows the Windows driver architecture with three main components:

1. **rcbottom** (`src/rc_bottom.c`): Hardware initialization and PCI device management
2. **rccfg** (`src/rc_config.c`): Configuration and management interface (`/dev/rcfg`)
3. **rcraid** (`src/rc_raid.c`): RAID array detection and SCSI host management

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test on TRX50 hardware
5. Submit a pull request

## License

GPL v2 License

## Troubleshooting

### Driver Loaded But No RAID Arrays Detected

If the driver loads successfully but doesn't show RAID arrays in `lsblk`:

```bash
# Check SCSI hosts
ls -la /sys/class/scsi_host/

# Check for any SCSI devices  
ls -la /sys/class/scsi_device/

# Check driver parameters
cat /sys/module/rcraid/parameters/* 2>/dev/null || echo "No parameters found"

# Check if RAID arrays are configured in BIOS
sudo dmesg | grep -i raid

# Check PCI device details
sudo lspci -vvv -s 4a:00.0 | grep -A 10 -B 5 "Region"

# Try rescanning for devices
echo "- - -" | sudo tee /sys/class/scsi_host/host*/scan 2>/dev/null || echo "No SCSI hosts found"

# Check again
lsblk
```

### Common Issues

1. **RAID arrays not configured in BIOS** - Ensure SATA mode is set to "RAID" and arrays are created
2. **SCSI headers missing** - Driver runs in limited mode without full SCSI support
3. **Wrong PCI BAR** - Driver automatically scans all BARs for MMIO space
4. **BIOS RAID configuration** - RAID arrays must be created in BIOS before driver can detect them

### Expected Output

**Working driver:**
```
rcraid: rc_init: AMD RAID Driver initialized successfully
rcraid: rc_init: Found 1 adapters
```

**RAID arrays should appear in:**
```bash
lsblk
# Should show your RAID arrays as block devices
```

## Support

- Create an issue on GitHub
- See [INSTALL.md](INSTALL.md) for detailed troubleshooting