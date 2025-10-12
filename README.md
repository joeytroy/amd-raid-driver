# AMD TRX50 RAID Driver

AMD RAID driver (rcraid) optimized specifically for TRX50 Threadripper PRO systems with NVMe RAID support.

## Overview

This repository provides a TRX50-optimized AMD RAID driver that supports:
- **TRX50 Threadripper PRO** systems (X399, WRX90, TRX50)
- **NVMe RAID arrays** configured in BIOS
- **SATA RAID arrays** on TRX50 platforms
- **AMD RAIDXpert** BIOS configurations
- **Cross-distribution** support for Ubuntu, Debian, Arch, RHEL, and SUSE

### Supported Hardware

**TRX50 Threadripper PRO Systems:**
- **TRX50** (Threadripper PRO 7000/9000 series)
- **WRX90** (Threadripper PRO 7000/9000 series)
- **X399** (Threadripper 1st/2nd gen)

**Drive Types:**
- **NVMe drives** (PCIe 3.0/4.0/5.0) - Primary focus
- **SATA drives** (SATA 3.0/6.0 Gbps) - Secondary support
- **Mixed configurations** (SATA + NVMe in same RAID array)

**RAID Levels:**
- **RAID 0** (Striping)
- **RAID 1** (Mirroring)
- **RAID 5** (Parity)
- **RAID 6** (Double Parity)

**Legacy Support:**
- Older AMD chipsets with RAID capabilities

### Supported Linux Distributions

#### **LTS (Long Term Support) Versions** ⭐ *Recommended for Production*

- **Ubuntu LTS** (including derivatives like Linux Mint, Pop!_OS, Kubuntu, etc.)
  - **Ubuntu 20.04 LTS** (Linux 5.4+) - Support until April 2025
  - **Ubuntu 22.04 LTS** (Linux 5.15+) - Support until April 2027
  - **Ubuntu 24.04 LTS** (Linux 6.8+) - Support until April 2029
- **Debian Stable** (including derivatives)
  - **Debian 11 (Bullseye)** (Linux 5.10+) - Support until August 2026
  - **Debian 12 (Bookworm)** (Linux 6.1+) - Support until June 2028
- **Red Hat Enterprise Linux** (Enterprise LTS)
  - **RHEL 8** (Linux 4.18+) - Support until May 2029
  - **RHEL 9** (Linux 5.14+) - Support until May 2032
- **SUSE Linux Enterprise** (Enterprise LTS)
  - **SLE 15 SP4/SP5** (Linux 5.14+) - Support until July 2027

#### **Generic Support**
- **Generic Linux** distributions with kernel 5.0+ (including Linux 6.8+, 6.9, 6.10, 6.11, 6.12, 6.13, 6.14, 6.15, 6.16, 6.17+)

## Documentation

- **[📖 Installation Guide](INSTALL.md)** - Complete installation instructions for all distributions
- **[🔧 Kernel Compatibility](docs/KERNEL_COMPATIBILITY.md)** - Version compatibility details

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test on multiple distributions
5. Submit a pull request

## License

This driver is based on proprietary AMD/Dot Hill Systems code. Please refer to the license headers in the source files for details.

## Disclaimer

This driver is provided as-is. Use at your own risk. Always backup your data before making changes to RAID configurations.

