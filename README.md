# AMD RAID Driver for Linux

A modern Linux kernel driver for AMD RAID controllers, built from scratch based on Windows driver specifications. This driver provides full support for AMD RAID arrays on TRX50 and other AMD platforms.

## Features

- **Complete Windows Driver Compatibility**: Built using exact specifications from Windows `rcbottom.inf`, `rccfg.inf`, and `rcraid.inf` files
- **TRX50 Platform Support**: Full support for AMD TRX50 Threadripper PRO platforms
- **Three-Layer Architecture**: 
  - `rcbottom`: Hardware initialization and PCI device management
  - `rccfg`: Configuration and management interface (`/dev/rcfg`)
  - `rcraid`: RAID array detection and SCSI host management
- **Power Management**: Full HIPM/DIPM and HMB allocation policy support
- **MSI/MSI-X Support**: Advanced interrupt handling with up to 5 vectors
- **SCSI Host Management**: Creates SCSI hosts for RAID array detection
- **Modern Linux Support**: Compatible with Kubuntu 24.04 LTS and newer kernels

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