# AMD RAID Driver

AMD RAID driver (rcraid) for modern Linux distributions supporting both SATA and NVMe drives.

## Overview

This repository provides a modern AMD RAID driver that supports:
- **SATA drives** in RAID configurations
- **NVMe drives** in RAID configurations  
- **AMD RAIDXpert** and **AMD Software RAID** BIOS configurations
- **Cross-distribution** support for Ubuntu, Debian, Arch, RHEL, and SUSE

### Supported Hardware

**Drive Types:**
- **SATA drives** (SATA 3.0/6.0 Gbps)
- **NVMe drives** (PCIe 3.0/4.0/5.0)

**AMD Chipsets:**

**AM4 Socket:**
- X370 / B350
- X399
- X470 / B450
- X570 / B550 / B550A
- X570S

**AM5 Socket:**
- X670 / X670E
- B650 / B650E
- A620
- X870E / B850E (2024+)

**Workstation/Server:**
- TRX50 (Threadripper PRO 7000/9000 series)
- WRX90 (Threadripper PRO 7000/9000 series)

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

